#include "stereo_wiener_48k.h"

#include <math.h>
#include <string.h>

enum {
    PCM24_BYTES = 3,
    STEREO_FRAME_BYTES = 2 * PCM24_BYTES,
    PCM24_MIN = -8388608,
    PCM24_MAX = 8388607,
    ESTIMATE_INTERVAL = 2,
    GAIN_STEP_FRAMES = 32,
};

_Static_assert(STEREO_WIENER_48K_FRAME_SAMPLES % GAIN_STEP_FRAMES == 0,
               "The gain ramp must span an integral number of blocks");

static inline int32_t unpack_pcm24(const uint8_t *pcm)
{
    const uint32_t bits = (uint32_t)pcm[0] | (uint32_t)pcm[1] << 8 | (uint32_t)pcm[2] << 16;
    return (int32_t)(bits ^ 0x800000u) - 0x800000;
}

static inline void pack_pcm24(uint8_t *pcm, float sample)
{
    int32_t rounded;
    if (sample >= (float)PCM24_MAX) {
        rounded = PCM24_MAX;
    } else if (sample <= (float)PCM24_MIN) {
        rounded = PCM24_MIN;
    } else {
        rounded = (int32_t)(sample + (sample < 0.0f ? -0.5f : 0.5f));
    }
    const uint32_t bits = (uint32_t)rounded;
    pcm[0] = (uint8_t)bits;
    pcm[1] = (uint8_t)(bits >> 8);
    pcm[2] = (uint8_t)(bits >> 16);
}

static void transform(stereo_wiener_48k_t *filter, bool inverse)
{
    stereo_wiener_48k_complex_t *data = filter->spectrum;
    for (unsigned i = 1, reversed = 0; i < STEREO_WIENER_48K_FFT_SIZE; ++i) {
        unsigned bit = STEREO_WIENER_48K_FFT_SIZE / 2;
        while (reversed & bit) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (i < reversed) {
            const stereo_wiener_48k_complex_t temp = data[i];
            data[i] = data[reversed];
            data[reversed] = temp;
        }
    }
    for (unsigned span = 2; span <= STEREO_WIENER_48K_FFT_SIZE; span *= 2) {
        const unsigned stride = STEREO_WIENER_48K_FFT_SIZE / span;
        const unsigned half = span / 2;
        for (unsigned start = 0; start < STEREO_WIENER_48K_FFT_SIZE; start += span) {
            for (unsigned k = 0; k < half; ++k) {
                const stereo_wiener_48k_complex_t w = filter->twiddle[k * stride];
                const float wi = inverse ? -w.imag : w.imag;
                const stereo_wiener_48k_complex_t a = data[start + k];
                const stereo_wiener_48k_complex_t b = data[start + k + half];
                const float real = w.real * b.real - wi * b.imag;
                const float imag = w.real * b.imag + wi * b.real;
                data[start + k] = (stereo_wiener_48k_complex_t){a.real + real, a.imag + imag};
                data[start + k + half] = (stereo_wiener_48k_complex_t){a.real - real, a.imag - imag};
            }
        }
    }
}

void stereo_wiener_48k_reset(stereo_wiener_48k_t *filter)
{
    memset(filter->history, 0, sizeof(filter->history));
    memset(filter->taps, 0, sizeof(filter->taps));
    memset(filter->target, 0, sizeof(filter->target));
    filter->taps[0] = 1.0f;
    filter->target[0] = 1.0f;
    filter->history_pos = 0;
    filter->frames_until_estimate = 0;
    filter->have_spectrum = false;
}

void stereo_wiener_48k_init(stereo_wiener_48k_t *filter)
{
    const float tau = 6.283185307179586f;
    for (unsigned i = 0; i < STEREO_WIENER_48K_FRAME_SAMPLES; ++i) {
        const float phase = (tau / (float)STEREO_WIENER_48K_FRAME_SAMPLES) * (float)i;
        filter->window[i] = 0.5f * (1.0f - cosf(phase)) / 8388608.0f;
    }
    for (unsigned i = 0; i < STEREO_WIENER_48K_FFT_SIZE / 2; ++i) {
        const float phase = (tau / (float)STEREO_WIENER_48K_FFT_SIZE) * (float)i;
        filter->twiddle[i] = (stereo_wiener_48k_complex_t){cosf(phase), -sinf(phase)};
    }
    stereo_wiener_48k_reset(filter);
}

static void load_spectrum(stereo_wiener_48k_t *filter, const uint8_t *packed_pcm24)
{
    /* A single complex FFT carries the two real input channels. */
    for (unsigned i = 0; i < STEREO_WIENER_48K_FRAME_SAMPLES; ++i) {
        const float window = filter->window[i];
        filter->spectrum[i] = (stereo_wiener_48k_complex_t){
            (float)unpack_pcm24(packed_pcm24) * window,
            (float)unpack_pcm24(packed_pcm24 + PCM24_BYTES) * window,
        };
        packed_pcm24 += STEREO_FRAME_BYTES;
    }
    memset(filter->spectrum + STEREO_WIENER_48K_FRAME_SAMPLES, 0,
           (STEREO_WIENER_48K_FFT_SIZE - STEREO_WIENER_48K_FRAME_SAMPLES)
               * sizeof(filter->spectrum[0]));
}

static void estimate_spectra(stereo_wiener_48k_t *filter)
{
    transform(filter, false);
    /* 1 - exp(-40 ms / 100 ms), for one estimate every two 20 ms frames. */
    const float update = 0.329680f;
    for (unsigned k = 0; k < STEREO_WIENER_48K_BINS; ++k) {
        const stereo_wiener_48k_complex_t a = filter->spectrum[k];
        const unsigned mirror = (STEREO_WIENER_48K_FFT_SIZE - k) & (STEREO_WIENER_48K_FFT_SIZE - 1);
        const stereo_wiener_48k_complex_t b = filter->spectrum[mirror];
        const stereo_wiener_48k_complex_t left = {
            0.5f * (a.real + b.real), 0.5f * (a.imag - b.imag),
        };
        const stereo_wiener_48k_complex_t right = {
            0.5f * (a.imag + b.imag), 0.5f * (b.real - a.real),
        };
        const stereo_wiener_48k_bin_t observed = {
            .left_power = left.real * left.real + left.imag * left.imag,
            .right_power = right.real * right.real + right.imag * right.imag,
            .cross = {
                left.real * right.real + left.imag * right.imag,
                left.imag * right.real - left.real * right.imag,
            },
        };
        stereo_wiener_48k_bin_t *bin = &filter->bins[k];
        if (!filter->have_spectrum) {
            *bin = observed;
        } else {
            bin->left_power += update * (observed.left_power - bin->left_power);
            bin->right_power += update * (observed.right_power - bin->right_power);
            bin->cross.real += update * (observed.cross.real - bin->cross.real);
            bin->cross.imag += update * (observed.cross.imag - bin->cross.imag);
        }
    }
    filter->have_spectrum = true;
}

static float wiener_gain(const stereo_wiener_48k_bin_t *bin)
{
    const float stronger = fmaxf(bin->left_power, bin->right_power);
    const float weaker = fminf(bin->left_power, bin->right_power);
    if (stronger < 1e-12f || weaker < 1e-3f * stronger) {
        return 1.0f;
    }

    /* Complex coherence preserves common content across microphone phase
     * differences. A dry floor protects weak speech and reflections. */
    const float cross_squared = bin->cross.real * bin->cross.real + bin->cross.imag * bin->cross.imag;
    const float coherence = fminf(1.0f, sqrtf(cross_squared / (stronger * weaker)));
    return 0.60f + 0.40f * coherence;
}

static void build_target(stereo_wiener_48k_t *filter)
{
    stereo_wiener_48k_complex_t *cepstrum = filter->spectrum;
    float current = wiener_gain(&filter->bins[0]);
    float previous = current;
    for (unsigned k = 0; k < STEREO_WIENER_48K_BINS; ++k) {
        const float next = k + 1 < STEREO_WIENER_48K_BINS ? wiener_gain(&filter->bins[k + 1]) : current;
        const float gain = 0.25f * previous + 0.5f * current + 0.25f * next;
        cepstrum[k] = (stereo_wiener_48k_complex_t){logf(gain), 0.0f};
        if (k > 0 && k < STEREO_WIENER_48K_FFT_SIZE / 2) {
            cepstrum[STEREO_WIENER_48K_FFT_SIZE - k] = cepstrum[k];
        }
        previous = current;
        current = next;
    }
    transform(filter, true);

    /* Use a causal, minimum-phase FIR. Truncate and taper the quiet tail. */
    float *target = filter->target;
    target[0] = expf(cepstrum[0].real / (float)STEREO_WIENER_48K_FFT_SIZE);
    for (unsigned i = 1; i < STEREO_WIENER_48K_TAPS; ++i) {
        float sum = 0.0f;
        for (unsigned k = 1; k <= i; ++k) {
            sum += (float)k * cepstrum[k].real * target[i - k];
        }
        target[i] = (2.0f / (float)STEREO_WIENER_48K_FFT_SIZE) * sum / (float)i;
    }
    const unsigned taper = STEREO_WIENER_48K_TAPS / 4;
    for (unsigned i = 0; i < taper; ++i) {
        target[STEREO_WIENER_48K_TAPS - taper + i] *= (float)(taper - 1 - i) / (float)taper;
    }
}

static void apply_filter(stereo_wiener_48k_t *filter, uint8_t *packed_pcm24)
{
    const float step = (float)GAIN_STEP_FRAMES / (float)STEREO_WIENER_48K_FRAME_SAMPLES;
    float tap_step[STEREO_WIENER_48K_TAPS];
    for (unsigned k = 0; k < STEREO_WIENER_48K_TAPS; ++k) {
        tap_step[k] = step * (filter->target[k] - filter->taps[k]);
    }

    unsigned pos = filter->history_pos;
    for (unsigned block = 0; block < STEREO_WIENER_48K_FRAME_SAMPLES; block += GAIN_STEP_FRAMES) {
        for (unsigned k = 0; k < STEREO_WIENER_48K_TAPS; ++k) {
            filter->taps[k] += tap_step[k];
        }
        for (unsigned i = 0; i < GAIN_STEP_FRAMES; ++i) {
            const float left = (float)unpack_pcm24(packed_pcm24);
            const float right = (float)unpack_pcm24(packed_pcm24 + PCM24_BYTES);
            filter->history[pos][0] = filter->history[pos + STEREO_WIENER_48K_TAPS][0] = left;
            filter->history[pos][1] = filter->history[pos + STEREO_WIENER_48K_TAPS][1] = right;
            const float (*history)[2] = filter->history + pos + STEREO_WIENER_48K_TAPS;

            float filtered_left = 0.0f;
            float filtered_right = 0.0f;
            for (int k = 0; k < STEREO_WIENER_48K_TAPS; ++k) {
                const float tap = filter->taps[k];
                filtered_left += tap * history[-k][0];
                filtered_right += tap * history[-k][1];
            }
            pack_pcm24(packed_pcm24, filtered_left);
            pack_pcm24(packed_pcm24 + PCM24_BYTES, filtered_right);
            packed_pcm24 += STEREO_FRAME_BYTES;
            pos = (pos + 1) & (STEREO_WIENER_48K_TAPS - 1);
        }
    }
    filter->history_pos = pos;
    memcpy(filter->taps, filter->target, sizeof(filter->taps));
}

void stereo_wiener_48k_process(stereo_wiener_48k_t *filter, uint8_t *packed_pcm24)
{
    if (filter->frames_until_estimate == 0) {
        load_spectrum(filter, packed_pcm24);
        apply_filter(filter, packed_pcm24);
        /* Publish gains for the next block, keeping the audio path causal. */
        estimate_spectra(filter);
        build_target(filter);
        filter->frames_until_estimate = ESTIMATE_INTERVAL - 1;
    } else {
        apply_filter(filter, packed_pcm24);
        --filter->frames_until_estimate;
    }
}

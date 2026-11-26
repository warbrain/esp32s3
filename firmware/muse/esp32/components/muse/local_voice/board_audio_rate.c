#include "board_audio_rate.h"

enum {
    PCM24_FRAME_BYTES = BOARD_AUDIO_RATE_CHANNELS * BOARD_AUDIO_PCM24_BYTES,
    PCM24_MIN = -8388608,
    PCM24_MAX = 8388607,
    PCM24_SHIFT = 8,
    FIR_SHIFT = 15,
};

/* Symmetric 7 kHz low-pass at 48 kHz, Kaiser window (beta = 4.7).
 * The Q15 coefficients sum to 32768 and suppress frequencies at and
 * above 8 kHz by at least 49 dB. Both conversion directions use this FIR. */
static const int16_t low_pass[BOARD_AUDIO_RATE_TAPS] = {
      8,   18,    15,    -8,   -35,   -40,    -7,    47,   78,   46,
    -41, -120,  -114,     0,   148,   205,    92,  -136, -303, -243,
     50,  376,   450,   142,  -375,  -694,  -478,   231,  944, 1023,
    180, -1163, -1989, -1272, 1312,  5004,  8259,  9548, 8259, 5004,
   1312, -1272, -1989, -1163,  180,  1023,   944,   231, -478, -694,
   -375,  142,   450,   376,    50,  -243,  -303,  -136,   92,  205,
    148,    0,  -114,  -120,   -41,    46,    78,    47,   -7,  -40,
    -35,   -8,    15,    18,     8,
};

static int32_t unpack_pcm24(const uint8_t *pcm)
{
    const uint32_t bits = (uint32_t)pcm[0] | (uint32_t)pcm[1] << 8 | (uint32_t)pcm[2] << 16;
    return (int32_t)(bits ^ 0x800000u) - 0x800000;
}

static void pack_pcm24(uint8_t *pcm, int32_t sample)
{
    const uint32_t bits = (uint32_t)sample;
    pcm[0] = (uint8_t)bits;
    pcm[1] = (uint8_t)(bits >> 8);
    pcm[2] = (uint8_t)(bits >> 16);
}

static inline int32_t round_shift(int64_t value, unsigned shift)
{
    const uint64_t half = UINT64_C(1) << (shift - 1);
    if (value < 0) {
        return -(int32_t)((0 - (uint64_t)value + half) >> shift);
    }
    return (int32_t)(((uint64_t)value + half) >> shift);
}

static int32_t clip_sample(int32_t sample, int32_t minimum, int32_t maximum)
{
    if (sample > maximum) {
        return maximum;
    }
    if (sample < minimum) {
        return minimum;
    }
    return sample;
}

void board_audio_unpack_capture(const uint8_t *packed_pcm24, int16_t *stereo_pcm16,
                                size_t hardware_frames)
{
    const size_t samples = hardware_frames * BOARD_AUDIO_RATE_CHANNELS;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = round_shift(unpack_pcm24(packed_pcm24), PCM24_SHIFT);
        *stereo_pcm16++ = (int16_t)clip_sample(sample, INT16_MIN, INT16_MAX);
        packed_pcm24 += BOARD_AUDIO_PCM24_BYTES;
    }
}

void board_audio_downsample_mono(board_audio_decimator_t *state, const uint8_t *packed_pcm24,
                                 int16_t *mono_pcm16, size_t voice_frames)
{
    size_t next = state->next;
    for (size_t frame = 0; frame < voice_frames; ++frame) {
        for (size_t phase = 0; phase < BOARD_AUDIO_RATE_RATIO; ++phase) {
            const int32_t sample = unpack_pcm24(packed_pcm24)
                + unpack_pcm24(packed_pcm24 + BOARD_AUDIO_PCM24_BYTES);
            state->history[next] = sample;
            state->history[next + BOARD_AUDIO_RATE_TAPS] = sample;
            packed_pcm24 += PCM24_FRAME_BYTES;
            if (++next == BOARD_AUDIO_RATE_TAPS) {
                next = 0;
            }
        }
        /* Mirroring the ring keeps the window contiguous. Pairing symmetric
         * taps halves the number of 64-bit products. */
        const int32_t *history = state->history + next;
        const size_t center = BOARD_AUDIO_RATE_TAPS / 2;
        int64_t sum = (int64_t)low_pass[center] * history[center];
        for (size_t tap = 0; tap < center; ++tap) {
            const int32_t pair = history[tap] + history[BOARD_AUDIO_RATE_TAPS - 1 - tap];
            sum += (int64_t)low_pass[tap] * pair;
        }
        /* The extra bit divides the stereo sum by two at final quantization. */
        const int32_t sample = round_shift(sum, FIR_SHIFT + PCM24_SHIFT + 1);
        *mono_pcm16++ = (int16_t)clip_sample(sample, INT16_MIN, INT16_MAX);
    }
    state->next = next;
}

void board_audio_upsample(board_audio_interpolator_t *state, const int16_t *mono_pcm16,
                          uint8_t *packed_pcm24, size_t voice_frames)
{
    size_t next = state->next;
    for (size_t frame = 0; frame < voice_frames; ++frame) {
        const int16_t input = *mono_pcm16++;
        state->history[next] = input;
        state->history[next + BOARD_AUDIO_RATE_PHASE_TAPS] = input;
        const int16_t *history = state->history + next + 1;

        /* Each output uses only the phase of the FIR that overlaps real
         * input samples. The x3 gain compensates for zero insertion.
         * Each phase's absolute coefficients sum to at most 20607, so
         * 3 * sum fits a signed 32-bit integer for any PCM16 input. */
        for (size_t phase = 0; phase < BOARD_AUDIO_RATE_RATIO; ++phase) {
            int32_t sum = 0;
            for (size_t tap = 0; tap < BOARD_AUDIO_RATE_PHASE_TAPS; ++tap) {
                const int32_t delayed = history[BOARD_AUDIO_RATE_PHASE_TAPS - 1 - tap];
                sum += (int32_t)low_pass[phase + tap * BOARD_AUDIO_RATE_RATIO] * delayed;
            }
            const int32_t sample = round_shift(3 * sum, FIR_SHIFT - PCM24_SHIFT);
            const int32_t clipped = clip_sample(sample, PCM24_MIN, PCM24_MAX);
            pack_pcm24(packed_pcm24, clipped);
            pack_pcm24(packed_pcm24 + BOARD_AUDIO_PCM24_BYTES, clipped);
            packed_pcm24 += PCM24_FRAME_BYTES;
        }
        if (++next == BOARD_AUDIO_RATE_PHASE_TAPS) {
            next = 0;
        }
    }
    state->next = next;
}

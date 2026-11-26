#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    STEREO_WIENER_48K_RATE = 48000,
    STEREO_WIENER_48K_FRAME_SAMPLES = STEREO_WIENER_48K_RATE / 50,
    STEREO_WIENER_48K_FFT_SIZE = 1024,
    STEREO_WIENER_48K_BINS = STEREO_WIENER_48K_FFT_SIZE / 2 + 1,
    STEREO_WIENER_48K_TAPS = 32,
};

typedef struct {
    float real;
    float imag;
} stereo_wiener_48k_complex_t;

typedef struct {
    float left_power;
    float right_power;
    stereo_wiener_48k_complex_t cross;
} stereo_wiener_48k_bin_t;

typedef struct {
    float window[STEREO_WIENER_48K_FRAME_SAMPLES];
    stereo_wiener_48k_complex_t twiddle[STEREO_WIENER_48K_FFT_SIZE / 2];
    stereo_wiener_48k_complex_t spectrum[STEREO_WIENER_48K_FFT_SIZE];
    stereo_wiener_48k_bin_t bins[STEREO_WIENER_48K_BINS];
    float taps[STEREO_WIENER_48K_TAPS];
    float target[STEREO_WIENER_48K_TAPS];
    float history[2 * STEREO_WIENER_48K_TAPS][2];
    unsigned history_pos;
    unsigned frames_until_estimate;
    bool have_spectrum;
} stereo_wiener_48k_t;

void stereo_wiener_48k_init(stereo_wiener_48k_t *filter);
void stereo_wiener_48k_reset(stereo_wiener_48k_t *filter);

/* Process one 20 ms frame of interleaved signed little-endian PCM24 in place. */
void stereo_wiener_48k_process(stereo_wiener_48k_t *filter, uint8_t *packed_pcm24);

#ifndef BOARD_AUDIO_RATE_H
#define BOARD_AUDIO_RATE_H

#include <stddef.h>
#include <stdint.h>

enum {
    BOARD_AUDIO_RATE_RATIO = 3,
    BOARD_AUDIO_RATE_CHANNELS = 2,
    BOARD_AUDIO_PCM24_BYTES = 3,
    BOARD_AUDIO_RATE_TAPS = 75,
    BOARD_AUDIO_RATE_PHASE_TAPS = BOARD_AUDIO_RATE_TAPS / BOARD_AUDIO_RATE_RATIO,
};

typedef struct {
    int32_t history[2 * BOARD_AUDIO_RATE_TAPS];
    size_t next;
} board_audio_decimator_t;

typedef struct {
    int16_t history[2 * BOARD_AUDIO_RATE_PHASE_TAPS];
    size_t next;
} board_audio_interpolator_t;

/* Quantize packed little-endian PCM24 to interleaved PCM16, both stereo at 48 kHz. */
void board_audio_unpack_capture(const uint8_t *packed_pcm24, int16_t *stereo_pcm16,
                                size_t hardware_frames);

/* Zero-initialize the state before each continuous stream. Each 16 kHz mono
 * voice sample corresponds to three 48 kHz stereo frames of packed PCM24. */
void board_audio_downsample_mono(board_audio_decimator_t *state, const uint8_t *packed_pcm24,
                                 int16_t *mono_pcm16, size_t voice_frames);
void board_audio_upsample(board_audio_interpolator_t *state, const int16_t *mono_pcm16,
                          uint8_t *packed_pcm24, size_t voice_frames);

#endif

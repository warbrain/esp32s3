#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

enum {
    BOARD_AUDIO_VOICE_RATE = 16000,
    BOARD_AUDIO_CAPTURE_RATE = 48000,
    BOARD_AUDIO_CAPTURE_CHANNELS = 2,
    BOARD_AUDIO_CAPTURE_SAMPLE_BYTES = 3,
    BOARD_AUDIO_FRAME_MS = 20,
    BOARD_AUDIO_FRAME_SAMPLES = BOARD_AUDIO_VOICE_RATE * BOARD_AUDIO_FRAME_MS / 1000,
    BOARD_AUDIO_CAPTURE_SAMPLES = BOARD_AUDIO_CAPTURE_RATE * BOARD_AUDIO_FRAME_MS / 1000,
    BOARD_AUDIO_CAPTURE_VALUES = BOARD_AUDIO_CAPTURE_SAMPLES * BOARD_AUDIO_CAPTURE_CHANNELS,
    BOARD_AUDIO_CAPTURE_BYTES = BOARD_AUDIO_CAPTURE_VALUES * BOARD_AUDIO_CAPTURE_SAMPLE_BYTES,
};

/* Open the 48 kHz/24-bit stereo hardware. The returned codec handles are
 * owned by the board and remain valid until close. */
esp_err_t board_audio_open(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *microphone);
/* Stop all codec users before closing the board audio path. */
void board_audio_close(void);

/* Read one packed little-endian PCM24 stereo block at 48 kHz. The optional VAD
 * copy is PCM16 mono at 16 kHz. The capture epoch changes after a discontinuity.
 * A single task owns each direction. Returns ESP_CODEC_DEV_* status codes. */
int board_audio_read(uint8_t pcm24[BOARD_AUDIO_CAPTURE_BYTES], int16_t *vad_mono, uint32_t *capture_epoch);
/* Pass NULL to send a timed block of silence. Other blocks are 16 kHz mono. */
int board_audio_write(const int16_t pcm[BOARD_AUDIO_FRAME_SAMPLES]);

/* Start or pause RX from the microphone task. TX keeps the shared I2S clocks.
 * Returns ESP_CODEC_DEV_* status codes. */
int board_audio_set_capture_active(bool active);
uint32_t board_audio_rx_overflows(void);

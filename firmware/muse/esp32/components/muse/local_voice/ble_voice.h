#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_VOICE_MAX_FRAME_BYTES 1612

/* Call once after nvs_flash_init(). Bonded peers can reconnect automatically. */
esp_err_t ble_voice_init(void);
/* Call from a physical board action. Require confirmation to replace a saved peer. */
bool ble_voice_open_pairing(bool replace_saved_peer);
/* Locally authorize the saved peer to replace a conflicting bond. */
bool ble_voice_repair_pairing(void);
/* Resume advertising or release an incomplete link while keeping the bond. */
bool ble_voice_retry_connection(void);
bool ble_voice_is_pairing(void);
bool ble_voice_has_saved_peer(void);
/* A peer is ready only after authorization, encryption and TX subscription. */
bool ble_voice_is_connected(void);
typedef struct {
    uint32_t tx;
    uint32_t rx;
    uint32_t tx_admission_drops;
    uint32_t tx_retries;
} ble_voice_audio_drops_t;
/* TX counts expired or failed audio frames after enqueue. RX counts receive
 * queue overflow. Admission counts rejected audio enqueue attempts, and
 * retries count transient transmit attempts (which may still succeed). */
ble_voice_audio_drops_t ble_voice_audio_drops(void);
/* Copy one complete A5 C3 frame to/from bounded queues. Not ISR APIs.
 * Send success means queued, not delivered; disconnect discards pending frames.
 * Receivers must provide BLE_VOICE_MAX_FRAME_BYTES of storage. */
esp_err_t ble_voice_send(const void *frame, size_t length, TickType_t timeout);
esp_err_t ble_voice_receive(void *frame, size_t capacity, size_t *length, TickType_t timeout);

#ifdef __cplusplus
}
#endif

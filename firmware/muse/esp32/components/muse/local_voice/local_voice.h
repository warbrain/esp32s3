#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The caller initializes NVS, display and the Wi-Fi station first. The local
 * voice engine is the sole owner of both I2S directions and the BLE host. */
esp_err_t local_voice_start(int volume, int mic_gain_db);
void local_voice_set_volume(int volume);
void local_voice_set_gain(int gain_db);
void local_voice_set_listening(bool enabled);
void local_voice_set_tts(bool enabled);
bool local_voice_set_ptt(bool pressed);

bool local_voice_connected(void);
bool local_voice_recording(void);
bool local_voice_listening(void);
bool local_voice_processing(void);
bool local_voice_speaking(void);

/* Expressions carry the same numeric values as the Windows Voice protocol. */
typedef enum {
    LOCAL_VOICE_EMOTION_NONE = 0,
    LOCAL_VOICE_EMOTION_HAPPY = 1,
    LOCAL_VOICE_EMOTION_CALM = 2,
    LOCAL_VOICE_EMOTION_SAD = 3,
    LOCAL_VOICE_EMOTION_TENSE = 4,
    LOCAL_VOICE_EMOTION_SURPRISED = 5,
    LOCAL_VOICE_EMOTION_ANGRY = 6,
} local_voice_emotion_t;
local_voice_emotion_t local_voice_emotion(void);

/* Local microphone metering alone never arms or sends a capture. */
void local_voice_set_monitor(bool enabled);
float local_voice_monitor_db(void);
void local_voice_request_chirp(void);

/* Pairing and replacement require an explicit physical action. */
bool local_voice_pair(bool replace);
bool local_voice_repair(void);
bool local_voice_retry(void);
bool local_voice_paired(void);
bool local_voice_pairing(void);
bool local_voice_ble_connected(void);
/* The Wi-Fi manager calls this from an event or disconnect handler. */
void local_voice_wifi_offline(void);

#ifdef __cplusplus
}
#endif

/* Connect the existing avatar and input tasks to the local PC audio bridge. */
#include "muse_voice.h"

#include <math.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/task.h"

#include "local_voice/local_voice.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_state.h"

static atomic_bool s_stopping;

static void show_voice_state(void)
{
    const muse_mode_t previous = muse_state_mode(NULL);
    if (previous == MUSE_MODE_BOOT) return;
    const muse_voice_view_t view = muse_voice_call_view();
    muse_mode_t mode = MUSE_MODE_IDLE;
    if (view == MUSE_VOICE_VIEW_LISTENING) mode = MUSE_MODE_LISTENING;
    else if (view == MUSE_VOICE_VIEW_THINKING) mode = MUSE_MODE_THINKING;
    else if (view == MUSE_VOICE_VIEW_SPEAKING) mode = MUSE_MODE_SPEAKING;

    if (previous != mode) {
        if (mode == MUSE_MODE_LISTENING && muse_state_asleep()) muse_state_set_asleep(false);
        muse_state_set_mode(mode);
        muse_state_poke();
    }
    const float db = local_voice_monitor_db();
    const float level = mode == MUSE_MODE_LISTENING ? fminf(fmaxf((db + 60.0f) / 40.0f, 0.0f), 1.0f) : 0.0f;
    muse_state_set_level(level);

    const muse_link_state_t link = local_voice_connected() ? MUSE_LINK_ONLINE : MUSE_LINK_OFFLINE;
    if (muse_link_state() != link) muse_link_set_state(link);
}

static void voice_input_task(void *arg)
{
    QueueHandle_t queue = arg;
    bool automatic = false;
    bool talk_held = false;
    int64_t wake_press_us = 0;
    uint32_t epoch = muse_input_call_epoch();

    for (;;) {
        muse_input_event_t event;
        const bool got_event = xQueueReceive(queue, &event, pdMS_TO_TICKS(30)) == pdTRUE;
        const uint32_t current = muse_input_call_epoch();
        const bool stopping = atomic_load(&s_stopping);
        if (current != epoch || stopping) {
            if (talk_held) local_voice_set_ptt(false);
            talk_held = false;
            wake_press_us = 0;
            epoch = current;
        }
        const bool arm = !stopping && muse_settings_auto_dictation() && muse_settings_ui_voice_active();
        if (arm != automatic) {
            automatic = arm;
            local_voice_set_listening(automatic);
        }
        if (got_event) {
            muse_input_clear_call_pending(event.mark);
            if (!stopping && event.epoch == epoch) {
                if (event.type == MUSE_PTT_DOWN) {
                    if (!talk_held && !wake_press_us) {
                        if (event.wake) wake_press_us = event.at_us;
                        else talk_held = local_voice_set_ptt(true);
                    }
                    if (!event.wake && !local_voice_connected()) muse_state_set_hint("CONNECT TO PC");
                } else {
                    wake_press_us = 0;
                    if (talk_held) local_voice_set_ptt(false);
                    talk_held = false;
                }
            }
        }
        if (wake_press_us && esp_timer_get_time() - wake_press_us >= 300000) {
            wake_press_us = 0;
            talk_held = local_voice_set_ptt(true);
        }
        if (!stopping) show_voice_state();
    }
}

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    if (!queue) return ESP_ERR_NO_MEM;
    const esp_err_t rc = local_voice_start(muse_settings_volume(), muse_settings_mic_gain());
    if (rc != ESP_OK) return rc;
    local_voice_set_tts(muse_settings_speaker_on());
    return xTaskCreate(voice_input_task, "voice_input", 3072, queue, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

muse_voice_view_t muse_voice_call_view(void)
{
    if (local_voice_recording()) return MUSE_VOICE_VIEW_LISTENING;
    if (local_voice_speaking()) return MUSE_VOICE_VIEW_SPEAKING;
    if (local_voice_processing()) return MUSE_VOICE_VIEW_THINKING;
    return MUSE_VOICE_VIEW_NONE;
}

bool muse_voice_in_call(void) { return muse_voice_call_view() != MUSE_VOICE_VIEW_NONE; }
bool muse_voice_ptt_call(void) { return false; }
bool muse_voice_ptt_enabled(void) { return true; }
const char *muse_voice_call_label(void) { return NULL; }
const char *muse_voice_call_hint(void) { return NULL; }

void muse_voice_shutdown(int wait_ms)
{
    atomic_store(&s_stopping, true);
    local_voice_set_listening(false);
    local_voice_set_ptt(false);
    const int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (local_voice_processing() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
}

void muse_voice_cancel_shutdown(void) { atomic_store(&s_stopping, false); }
void muse_voice_set_monitor(bool on) { local_voice_set_monitor(on); }
float muse_voice_monitor_db(void) { return local_voice_monitor_db(); }
void muse_voice_request_chirp(void) { local_voice_request_chirp(); }
bool muse_voice_resting(void) { return !muse_voice_in_call() && !local_voice_listening(); }
bool muse_voice_reply_waiting(void) { return false; }
bool muse_voice_notes_waiting(void) { return false; }

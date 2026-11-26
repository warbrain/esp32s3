/* Local Windows Voice transport for Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 * This module owns I2S/Opus and the S3-Voice NimBLE service. The host
 * application owns NVS, Wi-Fi station settings, display, and buttons.
 *
 * BLE and Wi-Fi UDP share a 12-byte little-endian header: A5 C3 magic,
 * packet type, flags, 16-bit sequence, 16-bit payload size, and board time.
 * Audio packets carry indexed 48 kHz stereo Opus uplink or 16 kHz mono
 * Opus readback. Indexed capture reports include offered frames and DMA
 * overflows; the first indexed frame can also carry the grant age.
 */
#include "local_voice.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include <pthread.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include "esp_wifi.h"
#include "esp_codec_dev.h"
#include "sdkconfig.h"
#include "opus.h"
#include "ble_voice.h"
#include "board_audio.h"
#include "board_audio_rate.h"
#include "stereo_wiener_48k.h"

static const char *TAG = "bridge";

/* Short confirmation tones mixed into the speaker stream. */
#define BEEP_MAX_SAMPLES 2048
static int16_t s_beep_up[BEEP_MAX_SAMPLES];
static int16_t s_beep_down[BEEP_MAX_SAMPLES];
static int s_beep_up_len;
static int s_beep_down_len;
enum speaker_beep { SPEAKER_BEEP_NONE, SPEAKER_BEEP_START, SPEAKER_BEEP_END };
static atomic_int s_pending_beep;

/* Telemetry samples these counters across tasks and CPU cores. */
static atomic_uint s_up_frames;
static atomic_uint s_up_bytes;
static atomic_uint s_up_swb_fb_frames;
static atomic_uint s_wifi_catchup_frames;
static atomic_uint s_down_frames;
static atomic_uint s_down_idle;
static atomic_uint s_down_rebuffers;
static atomic_uint s_down_dropped;
static atomic_uint s_speaker_errors;
static atomic_uint s_mic_frames;
static atomic_uint s_mic_errors;
static atomic_uint s_mic_dropped;
static atomic_int s_mic_last_error;
static atomic_uint s_mic_gap_ms;
static atomic_uint s_up_packet_peak;
static atomic_uint s_encode_us;
static atomic_uint s_encode_peak_us;
static atomic_uint s_encode_late;
static atomic_uint s_wiener_us;
static atomic_uint s_uplink_work_peak_us;
static atomic_uint s_uplink_work_late;
static atomic_uint s_uplink_send_us;
static atomic_uint s_wifi_audio_pressure_ms;
static atomic_uint s_opus_lock_peak_us;
static atomic_uint s_up_queue_peak_us;
static atomic_uint s_encode_errors;
static atomic_uint s_send_errors;
static atomic_uint s_decode_errors;

#define SERVER_PORT 5000

#define VOICE_RATE      BOARD_AUDIO_VOICE_RATE
#define UPLINK_RATE     BOARD_AUDIO_CAPTURE_RATE
#define CHANNELS        BOARD_AUDIO_CAPTURE_CHANNELS
#define FRAME_SAMPLES   BOARD_AUDIO_FRAME_SAMPLES
#define UPLINK_SAMPLES  BOARD_AUDIO_CAPTURE_SAMPLES
#define HDR_BYTES       12


#define FRAME_MS        BOARD_AUDIO_FRAME_MS
#define OPUS_MAX_PACKET 512
#define OPUS_UP_BITRATE 48000
#define OPUS_UP_COMPLEXITY 1
#define AUDIO_CORE      (CONFIG_FREERTOS_NUMBER_OF_CORES - 1)
#define AUDIO_STARTUP_DONE BIT0
#define PSRAM_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PLAYBACK_PREFILL_FRAMES 5
#define PLAYBACK_PREFILL_US     (PLAYBACK_PREFILL_FRAMES * FRAME_MS * 1000)
#define PLAYBACK_RESUME_US      200000

#define VAD_PREROLL_FRAMES     (240 / FRAME_MS)
#define VAD_ONSET_FRAMES       (100 / FRAME_MS)
#define VAD_SILENCE_FRAMES     (900 / FRAME_MS)
#define VAD_SILENCE_KEEP_FRAMES (200 / FRAME_MS)
#define VAD_ONSET_HOLD_MS      600
#define VAD_START_QUEUE_MS     300
#define VAD_BLE_GRANT_MS       750
#define VAD_WIFI_GRANT_MS      2200
#define VAD_BLE_START_RETRY_MS 180
#define VAD_WIFI_START_RETRY_MS 140
/* Preserve the pre-roll and detected onset until the Wi-Fi grant deadline. */
#define VAD_BUFFER_FRAMES      (3200 / FRAME_MS)
#define VAD_SPEAKER_GUARD_MS   300
#define VAD_STATUS_POLL_MS     250
#define VAD_BLE_STATUS_LEASE_MS 850
#define VAD_WIFI_STATUS_LEASE_MS 1800
#define VAD_RX_WARMUP_FRAMES   (100 / FRAME_MS)
/* Require headroom on the last successful send before increasing the Wi-Fi rate. */
#define WIFI_CATCHUP_SEND_US   (FRAME_MS * 750)
#define WIFI_CATCHUP_COOLDOWN_MS 250
#define WIFI_AUDIO_RETRY_WINDOW_US 2000
#define MIC_READ_FAILURE_LIMIT 3
#define VOICE_EMOTION_HOLD_MS   4000
#define VOICE_EMOTION_RESULT_MS 8000

_Static_assert((int)UPLINK_RATE == STEREO_WIENER_48K_RATE &&
               (int)UPLINK_SAMPLES == STEREO_WIENER_48K_FRAME_SAMPLES,
               "Wiener requires one native I2S frame per Opus frame");
_Static_assert(VAD_BUFFER_FRAMES * FRAME_MS >= VAD_WIFI_GRANT_MS + VAD_ONSET_HOLD_MS + VAD_PREROLL_FRAMES * FRAME_MS,
               "VAD buffering must cover the pre-roll and grant deadline");

enum { T_HELLO = 1, T_AUDIO_UP = 2, T_AUDIO_DOWN = 3, T_CTRL = 4, T_STATUS = 5 };
enum { CAPTURE_DELIVERY_FLAG = 0x02, VAD_GRANT_AGE_FLAG = 0x04, CAPTURE_REPORT_FLAG = 0x10 };
enum { CAPTURE_REPORT_TAIL_MS = 60, CAPTURE_REPORT_RETRY_MS = 180, CAPTURE_REPORT_DEADLINE_MS = 1800 };
enum { UDP_IDLE_RECEIVE_MS = 200, UDP_ACTIVE_RECEIVE_MS = 30 };
enum mic_item_type { MIC_AUDIO, MIC_AUTO_START, MIC_CAPTURE_END };
enum capture_mode { CAPTURE_MANUAL, CAPTURE_AUTO_WAITING, CAPTURE_AUTO };
enum auto_capture_stop { AUTO_STOP_CHECK_STATE, AUTO_STOP_SILENCE, AUTO_STOP_AUDIO_ERROR };
enum capture_end_reason { CAPTURE_RELEASED, CAPTURE_SILENCE, CAPTURE_PAGE_CHANGED,
                          CAPTURE_GRANT_TIMEOUT, CAPTURE_AUDIO_ERROR };

typedef struct {
    uint16_t offered_frames;
    uint16_t dma_overflows;
} capture_delivery_t;

typedef struct {
    unsigned epoch;
    unsigned capture_id;
    uint32_t queued_us;
    uint32_t sample_started_ms;
    uint32_t pcm_epoch;
    unsigned page_epoch;
    enum mic_item_type type;
    union {
        struct {
            uint16_t audio_sequence;
            uint16_t grant_age_ms;
        };
        capture_delivery_t delivery;
        struct {
            uint16_t buffered_frames;
            uint16_t held_ms;
        } onset;
    };
    uint8_t pcm24[BOARD_AUDIO_CAPTURE_BYTES];
} mic_frame_t;

#define MIC_CONTROL_SIZE offsetof(mic_frame_t, pcm24)

static int s_sock = -1;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_capture_lock;
static SemaphoreHandle_t s_volume_lock;
static RingbufHandle_t s_up_ring;    /* microphone PCM -> Opus sender */
static RingbufHandle_t s_down_ring;  /* incoming Opus -> speaker */
static mic_frame_t *s_mic_frame;     /* capture scratch in PSRAM */
static struct {
    stereo_wiener_48k_t wiener;
    int16_t opus_pcm[BOARD_AUDIO_CAPTURE_VALUES];
} *s_uplink_dsp;
static esp_codec_dev_handle_t s_spk, s_mic;
static OpusEncoder *s_enc;   /* 48 kHz, 2 ch: both microphones */
static OpusDecoder *s_dec;   /* 16 kHz, 1 ch: TTS downlink */
static atomic_int s_volume;
static atomic_bool s_monitor_enabled;
static atomic_int s_mic_level_centidb = -10000;
static SemaphoreHandle_t s_opus_lock;   /* micro-opus runs a single pseudostack */
static atomic_bool s_audio_ready;
static atomic_bool s_talking;
static atomic_int s_capture_mode;
static atomic_bool s_voice_page_open;
static atomic_uint s_voice_page_epoch;
static atomic_bool s_vad_available;
static atomic_bool s_vad_listening;
static atomic_bool s_vad_host_busy;
static atomic_uint s_vad_ready_until_ms;
static atomic_uint s_vad_started_ms;
static uint32_t s_vad_start_retry_ms;   /* Protected by s_capture_lock. */
static unsigned s_vad_start_attempts;
static atomic_uint s_vad_grant_ms;
static atomic_uint s_vad_capture_page_epoch;
static atomic_uint s_vad_ended_ms;
static atomic_uint s_voice_emotion_ms;
static atomic_int s_voice_emotion;
static bool s_voice_emotion_capture;  /* Protected by s_capture_lock. */
static atomic_uint s_speaker_active_ms;
static atomic_uint s_readback_queued_ms;
static atomic_uint s_readback_active_ms;
static atomic_bool s_tts_enabled;
static atomic_uint s_capture_epoch;
static atomic_uint s_capture_id;
static atomic_uint s_capture_dma_start;
static atomic_uint s_down_epoch;
static atomic_bool s_link_up;
static atomic_bool s_ble_link_up;
static atomic_bool s_release_pending;
static bool s_release_ready;           /* Protected by s_capture_lock. */
static bool s_release_delivery_sent;
static capture_delivery_t s_release_delivery;
static enum capture_end_reason s_capture_end_reason;  /* Protected by s_capture_lock. */
static int64_t s_release_retry_ms;
static bool s_capture_report;         /* Negotiated Wi-Fi report, protected by s_capture_lock. */
static struct {
    unsigned id;
    capture_delivery_t delivery;
    int64_t retry_ms;
    int64_t deadline_ms;
} s_wifi_report;                      /* Last capture report, independent of the microphone. */
static struct sockaddr_in s_peer;      /* registered by a UDP ping */
static int64_t s_last_beacon_ms = 0;
static uint16_t s_seq = 0;

static void clear_audio_ring(RingbufHandle_t ring);
static void reset_voice_audio(void);

static void reset_downlink_audio(void)
{
    atomic_fetch_add(&s_down_epoch, 1);
    clear_audio_ring(s_down_ring);
}

/* ---------------------------------------------------------------- transport */

static bool voice_link_up(void)
{
    return atomic_load(&s_ble_link_up) || atomic_load(&s_link_up);
}

static uint32_t voice_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t vad_grant_timeout_ms(void)
{
    return atomic_load(&s_ble_link_up) ? VAD_BLE_GRANT_MS : VAD_WIFI_GRANT_MS;
}

static uint32_t vad_start_retry_interval_ms(void)
{
    return atomic_load(&s_ble_link_up) ? VAD_BLE_START_RETRY_MS : VAD_WIFI_START_RETRY_MS;
}

static bool vad_host_ready(uint32_t now_ms)
{
    const uint32_t until = s_vad_ready_until_ms;
    return until != 0 && (int32_t)(until - now_ms) > 0;
}

static bool vad_speaker_quiet(void)
{
    const uint32_t active = s_speaker_active_ms;
    const uint32_t queued = s_readback_queued_ms;
    const uint32_t now_ms = voice_now_ms();
    return atomic_load_explicit(&s_pending_beep, memory_order_relaxed) == SPEAKER_BEEP_NONE &&
        (active == 0 || now_ms - active >= VAD_SPEAKER_GUARD_MS) &&
        (queued == 0 || now_ms - queued >= VAD_SPEAKER_GUARD_MS);
}

static bool mic_capture_current(const mic_frame_t *frame)
{
    return (atomic_load(&s_talking) || atomic_load(&s_release_pending))
        && frame->epoch == atomic_load(&s_capture_epoch)
        && frame->capture_id == atomic_load(&s_capture_id);
}

static bool udp_send_has_pressure(int error)
{
    return error == ENOMEM || error == ENOBUFS || error == EAGAIN || error == EWOULDBLOCK;
}

static esp_err_t send_frame(uint8_t type, uint8_t flags, const void *payload, uint16_t len,
                            const mic_frame_t *mic_frame, int *udp_error, int64_t wifi_retry_deadline_us)
{
    if (udp_error != NULL) *udp_error = 0;
    if (!voice_link_up()) {
        return ESP_FAIL;
    }
    // Shared scratch for all senders holding s_tx_lock.
    static uint8_t packet[BLE_VOICE_MAX_FRAME_BYTES];
    if (len > sizeof(packet) - HDR_BYTES) {
        return ESP_FAIL;
    }
    const bool wifi_retry = wifi_retry_deadline_us != 0;
    if (xSemaphoreTake(s_tx_lock, wifi_retry ? 0 : pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_FAIL;
    }
    // Check under the same lock that serializes the PTT control packets.
    if (mic_frame != NULL && !mic_capture_current(mic_frame)) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_retry && (atomic_load(&s_ble_link_up) || !atomic_load(&s_link_up))) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
    const uint16_t sequence = mic_frame != NULL ? mic_frame->audio_sequence : s_seq;
    if (mic_frame != NULL && sequence == 0 && mic_frame->grant_age_ms != UINT16_MAX) {
        flags |= VAD_GRANT_AGE_FLAG;
        ts = mic_frame->grant_age_ms;
    }
    packet[0] = 0xA5;
    packet[1] = 0xC3;
    packet[2] = type;
    packet[3] = flags;
    packet[4] = (uint8_t)(sequence & 0xFF);
    packet[5] = (uint8_t)(sequence >> 8);
    packet[6] = (uint8_t)(len & 0xFF);
    packet[7] = (uint8_t)(len >> 8);
    packet[8] = (uint8_t)(ts & 0xFF);
    packet[9] = (uint8_t)((ts >> 8) & 0xFF);
    packet[10] = (uint8_t)((ts >> 16) & 0xFF);
    packet[11] = (uint8_t)((ts >> 24) & 0xFF);
    if (len > 0) {
        memcpy(packet + HDR_BYTES, payload, len);
    }

    s_seq++;
    esp_err_t rc = ESP_FAIL;
    if (!wifi_retry && atomic_load(&s_ble_link_up)) {
        const TickType_t wait = type == T_AUDIO_UP ? 0 : pdMS_TO_TICKS(30);
        rc = ble_voice_send(packet, HDR_BYTES + len, wait) == ESP_OK ? ESP_OK : ESP_FAIL;
    } else if (atomic_load(&s_link_up) && (!wifi_retry || esp_timer_get_time() < wifi_retry_deadline_us)) {
        const int sent = sendto(s_sock, packet, HDR_BYTES + len, 0,
                                (const struct sockaddr *)&s_peer, sizeof(s_peer));
        const int error = sent < 0 ? errno : 0;
        if (udp_error != NULL) *udp_error = error;
        if (type == T_AUDIO_UP && udp_send_has_pressure(error)) {
            atomic_store_explicit(&s_wifi_audio_pressure_ms, voice_now_ms(), memory_order_relaxed);
        }
        rc = sent == HDR_BYTES + len ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_tx_lock);
    return rc;
}

static bool send_ctrl(const char *text)
{
    return send_frame(T_CTRL, 0, text, (uint16_t)strlen(text), NULL, NULL, 0) == ESP_OK;
}

static bool send_tts_state(void)
{
    return send_ctrl(atomic_load(&s_tts_enabled) ? "tts:1" : "tts:0");
}

static bool send_vad_capture_control(const char *command, unsigned capture_id, int *udp_error)
{
    char text[32];
    snprintf(text, sizeof(text), "%s:%u", command, capture_id);
    const uint8_t flags = !atomic_load(&s_ble_link_up) && strcmp(command, "vad_start") == 0 ? CAPTURE_REPORT_FLAG : 0;
    return send_frame(T_CTRL, flags, text, (uint16_t)strlen(text), NULL, udp_error, 0) == ESP_OK;
}

static void complete_capture_release_locked(bool automatic)
{
    s_release_ready = false;
    s_vad_ready_until_ms = 0;
    s_vad_ended_ms = voice_now_ms();
    if (automatic) s_vad_host_busy = true;
    s_release_pending = false;
    ESP_LOGI(TAG, "-> %s", automatic ? "vad_end" : "ptt_up");
}

static void send_wifi_report_locked(int64_t now_ms)
{
    if (s_wifi_report.deadline_ms == 0 || now_ms < s_wifi_report.retry_ms) return;
    const unsigned id = s_wifi_report.id;
    const capture_delivery_t delivery = s_wifi_report.delivery;
    if (now_ms >= s_wifi_report.deadline_ms) {
        ESP_LOGW(TAG, "Wi-Fi capture report timed out: id=%u offered=%u", id, (unsigned)delivery.offered_frames);
        s_wifi_report.deadline_ms = 0;
        return;
    }
    // One datagram carries the capture ID and final count. Voice can acknowledge a duplicate
    // without transcribing twice, while retransmission leaves the microphone free to listen.
    const uint8_t payload[] = {
        (uint8_t)delivery.offered_frames, (uint8_t)(delivery.offered_frames >> 8),
        (uint8_t)delivery.dma_overflows, (uint8_t)(delivery.dma_overflows >> 8),
        (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24),
    };
    s_wifi_report.retry_ms = now_ms + CAPTURE_REPORT_RETRY_MS;
    if (atomic_load(&s_link_up)) {
        send_frame(T_STATUS, CAPTURE_DELIVERY_FLAG | CAPTURE_REPORT_FLAG, payload, sizeof(payload), NULL, NULL, 0);
    }
}

static void send_capture_release_locked(int64_t now_ms)
{
    if (s_release_pending && s_release_ready && voice_link_up() && now_ms >= s_release_retry_ms) {
        const unsigned id = s_capture_id;
        if (s_capture_report) {
            s_wifi_report.id = id;
            s_wifi_report.delivery = s_release_delivery;
            s_wifi_report.retry_ms = now_ms + CAPTURE_REPORT_TAIL_MS;
            s_wifi_report.deadline_ms = now_ms + CAPTURE_REPORT_DEADLINE_MS;
            complete_capture_release_locked(true);
            return;
        }

        s_release_retry_ms = now_ms + 60;
        if (!s_release_delivery_sent) {
            const uint8_t payload[] = {
                (uint8_t)s_release_delivery.offered_frames, (uint8_t)(s_release_delivery.offered_frames >> 8),
                (uint8_t)s_release_delivery.dma_overflows, (uint8_t)(s_release_delivery.dma_overflows >> 8),
            };
            if (send_frame(T_STATUS, CAPTURE_DELIVERY_FLAG, payload, sizeof(payload), NULL, NULL, 0) != ESP_OK) return;
            s_release_delivery_sent = true;
        }
        const bool automatic = s_capture_mode != CAPTURE_MANUAL;
        const bool sent = automatic ? send_vad_capture_control("vad_end", id, NULL) : send_ctrl("ptt_up");
        if (sent) complete_capture_release_locked(automatic);
    }
}

static void retry_capture_release(int64_t now_ms)
{
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    send_capture_release_locked(now_ms);
    send_wifi_report_locked(now_ms);
    xSemaphoreGive(s_capture_lock);
}

/* --------------------------------------------------------------------- opus */

static esp_err_t opus_start(void)
{
    int err = OPUS_OK;
    /* Force CELT for the uplink to leave enough time for the PCM24 Wiener.
     * The receiver uses Opus packet-loss concealment with CELT packets. */
    s_enc = opus_encoder_create(UPLINK_RATE, CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK || s_enc == NULL) {
        ESP_LOGE(TAG, "opus encoder create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    if (opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_UP_BITRATE)) != OPUS_OK ||
        opus_encoder_ctl(s_enc, OPUS_SET_FORCE_CHANNELS(CHANNELS)) != OPUS_OK ||
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_UP_COMPLEXITY)) != OPUS_OK ||
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(5)) != OPUS_OK ||
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) {
        ESP_LOGE(TAG, "configure Opus uplink failed");
        return ESP_FAIL;
    }

    s_dec = opus_decoder_create(VOICE_RATE, 1, &err);
    if (err != OPUS_OK || s_dec == NULL) {
        ESP_LOGE(TAG, "opus decoder create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "opus ready: uplink %d Hz stereo CELT, %d bps, complexity %d; downlink %d Hz mono",
             UPLINK_RATE, OPUS_UP_BITRATE, OPUS_UP_COMPLEXITY, VOICE_RATE);
    return ESP_OK;
}

static int beep_build(int16_t *out, int capacity, int freq_hz, int ms)
{
    const int count = (ms * 16000) / 1000;
    if (count > capacity) {
        return capacity;
    }
    const int ramp = count / 6; /* short fade in and out, no click */
    for (int i = 0; i < count; i++) {
        const double phase = 2.0 * 3.14159265358979 * freq_hz * i / 16000.0;
        double gain = 0.22;
        if (i < ramp) gain *= (double)i / ramp;
        if (i > count - ramp) gain *= (double)(count - i) / ramp;
        out[i] = (int16_t)(sin(phase) * gain * 32767.0);
    }
    return count;
}

static void beep_init(void)
{
    s_beep_up_len = beep_build(s_beep_up, BEEP_MAX_SAMPLES, 880, 70);
    s_beep_down_len = beep_build(s_beep_down, BEEP_MAX_SAMPLES, 587, 90);
}

/* Called under the capture lock on the transition to recording. */
static void start_capture_locked(enum capture_mode mode)
{
    clear_audio_ring(s_up_ring);
    reset_downlink_audio();
    atomic_fetch_add(&s_capture_id, 1);
    s_voice_emotion_capture = false;
    atomic_store(&s_voice_emotion, LOCAL_VOICE_EMOTION_NONE);
    s_capture_dma_start = board_audio_rx_overflows();
    s_vad_ready_until_ms = 0;
    atomic_store(&s_vad_grant_ms, 0);
    atomic_store_explicit(&s_uplink_send_us, 0, memory_order_relaxed);
    s_capture_report = false;
    s_capture_mode = mode;
    atomic_store(&s_talking, true);
}

static void finish_capture_locked(enum capture_end_reason reason)
{
    /* The mic appends an end marker after its queued audio. */
    s_capture_end_reason = reason;
    s_release_ready = false;
    s_release_pending = true;
    s_talking = false;
}

/* Serialize manual capture with peer changes and remote stops. */
bool local_voice_set_ptt(bool active)
{
    if (!s_audio_ready) return false;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    bool changed = false;
    if (active) {
        if (!s_talking && !s_release_pending && voice_link_up() &&
            send_tts_state() && send_ctrl("ptt_down")) {
            start_capture_locked(CAPTURE_MANUAL);
            changed = true;
            ESP_LOGI(TAG, "-> ptt_down");
        }
    } else if (s_talking && s_capture_mode == CAPTURE_MANUAL) {
        finish_capture_locked(CAPTURE_RELEASED);
        changed = true;
    }
    xSemaphoreGive(s_capture_lock);
    return changed;
}

static bool wait_for_audio_startup(EventGroupHandle_t startup)
{
    xEventGroupWaitBits(startup, AUDIO_STARTUP_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    return s_audio_ready;
}

typedef struct {
    uint32_t sample_started_ms;
    uint32_t pcm_epoch;
    uint8_t pcm24[BOARD_AUDIO_CAPTURE_BYTES];
} vad_buffered_frame_t;

typedef struct {
    vad_handle_t detector;
    vad_buffered_frame_t *buffer;
    unsigned head;
    unsigned count;
    unsigned voiced_frames;
    unsigned silent_frames;
    uint32_t onset_ms;
    uint32_t request_ms;
} auto_capture_t;

typedef struct {
    unsigned id;
    uint16_t offered_frames;
    uint32_t last_wifi_catchup_us;
} mic_capture_progress_t;

static void clear_auto_audio(auto_capture_t *capture)
{
    capture->head = 0;
    capture->count = 0;
    capture->voiced_frames = 0;
    capture->silent_frames = 0;
    capture->onset_ms = 0;
    capture->request_ms = 0;
}

static void buffer_auto_audio(auto_capture_t *capture, const mic_frame_t *frame, unsigned limit)
{
    if (capture->count >= limit) {
        const unsigned discard = capture->count + 1 - limit;
        capture->head = (capture->head + discard) % VAD_BUFFER_FRAMES;
        capture->count -= discard;
    }
    const unsigned next = (capture->head + capture->count) % VAD_BUFFER_FRAMES;
    vad_buffered_frame_t *slot = &capture->buffer[next];
    memcpy(slot->pcm24, frame->pcm24, sizeof(frame->pcm24));
    slot->sample_started_ms = frame->sample_started_ms;
    slot->pcm_epoch = frame->pcm_epoch;
    capture->count++;
}

enum mic_queue_policy { MIC_DROP_IF_FULL, MIC_KEEP_IF_FULL };

static bool queue_mic_audio(mic_frame_t *frame, mic_capture_progress_t *progress,
                            enum mic_queue_policy policy)
{
    if (!mic_capture_current(frame)) return false;
    if (frame->capture_id != progress->id) {
        *progress = (mic_capture_progress_t){.id = frame->capture_id};
    }
    frame->type = MIC_AUDIO;
    frame->audio_sequence = progress->offered_frames;
    frame->grant_age_ms = UINT16_MAX;
    if (frame->audio_sequence == 0 && frame->sample_started_ms != 0) {
        const uint32_t grant_ms = atomic_load(&s_vad_grant_ms);
        if (grant_ms != 0) {
            /* Unsigned subtraction also handles wrap of the monotonic millisecond counter. */
            const uint32_t before_ms = grant_ms - frame->sample_started_ms;
            if (before_ms < UINT16_MAX) {
                frame->grant_age_ms = (uint16_t)before_ms;
            } else if (frame->sample_started_ms - grant_ms < UINT16_MAX) {
                frame->grant_age_ms = 0;
            }
        }
    }
    frame->queued_us = (uint32_t)esp_timer_get_time();
    const bool queued = xRingbufferSend(s_up_ring, frame, sizeof(*frame), 0) == pdTRUE;
    if (!queued) {
        if (policy == MIC_KEEP_IF_FULL) return false;
        atomic_fetch_add_explicit(&s_mic_dropped, 1, memory_order_relaxed);
    }
    ++progress->offered_frames;
    atomic_fetch_add_explicit(&s_mic_frames, 1, memory_order_relaxed);
    return queued;
}

static bool queue_buffered_audio(auto_capture_t *capture, mic_frame_t *frame, mic_capture_progress_t *progress,
                                 enum mic_queue_policy policy)
{
    const vad_buffered_frame_t *slot = &capture->buffer[capture->head];
    memcpy(frame->pcm24, slot->pcm24, sizeof(frame->pcm24));
    frame->sample_started_ms = slot->sample_started_ms;
    frame->pcm_epoch = slot->pcm_epoch;
    const bool queued = queue_mic_audio(frame, progress, policy);
    if (queued || policy == MIC_DROP_IF_FULL) {
        capture->head = (capture->head + 1) % VAD_BUFFER_FRAMES;
        capture->count--;
    }
    return queued;
}

static void send_auto_audio(auto_capture_t *capture, mic_frame_t *frame, mic_capture_progress_t *progress)
{
    if (!queue_buffered_audio(capture, frame, progress, MIC_DROP_IF_FULL) || capture->count == 0 ||
        !atomic_load(&s_link_up) || atomic_load(&s_ble_link_up)) return;

    const uint32_t pressure_ms = atomic_load_explicit(&s_wifi_audio_pressure_ms, memory_order_relaxed);
    if (pressure_ms != 0 && voice_now_ms() - pressure_ms < WIFI_CATCHUP_COOLDOWN_MS) return;

    // A Wi-Fi grant can leave older PCM buffered. Offer at most one extra
    // frame per iteration while the sender has room and can keep up.
    const unsigned send_us = atomic_load_explicit(&s_uplink_send_us, memory_order_relaxed);
    if (send_us == 0 || send_us > WIFI_CATCHUP_SEND_US) return;
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    if (progress->last_wifi_catchup_us != 0 &&
        now_us - progress->last_wifi_catchup_us < FRAME_MS * 1000) return;
    UBaseType_t pending = 0;
    vRingbufferGetInfo(s_up_ring, NULL, NULL, NULL, NULL, &pending);
    if (pending <= 1 && queue_buffered_audio(capture, frame, progress, MIC_KEEP_IF_FULL)) {
        progress->last_wifi_catchup_us = now_us;
        atomic_fetch_add_explicit(&s_wifi_catchup_frames, 1, memory_order_relaxed);
    }
}

static bool vad_capture_page_current(void)
{
    return s_voice_page_open && s_voice_page_epoch == s_vad_capture_page_epoch;
}

static bool auto_capture_should_stop(enum capture_mode mode, uint32_t now_ms)
{
    return !vad_capture_page_current() ||
        (mode == CAPTURE_AUTO_WAITING && now_ms - s_vad_started_ms >= vad_grant_timeout_ms());
}

static const char *capture_end_reason_name(enum capture_end_reason reason)
{
    switch (reason) {
    case CAPTURE_RELEASED: return "released";
    case CAPTURE_SILENCE: return "silence";
    case CAPTURE_PAGE_CHANGED: return "page_changed";
    case CAPTURE_GRANT_TIMEOUT: return "grant_timeout";
    case CAPTURE_AUDIO_ERROR: return "audio_error";
    }
    return "unknown";
}

static bool finish_auto_capture(const mic_frame_t *frame, enum auto_capture_stop stop)
{
    if (xSemaphoreTake(s_capture_lock, 0) != pdTRUE) return false;
    const enum capture_mode mode = s_capture_mode;
    bool trim_silence = false;
    if (s_talking && mode != CAPTURE_MANUAL && mic_capture_current(frame)) {
        const uint32_t now_ms = voice_now_ms();
        const bool forced = auto_capture_should_stop(mode, now_ms);
        if (stop != AUTO_STOP_CHECK_STATE || forced) {
            enum capture_end_reason reason = CAPTURE_SILENCE;
            if (!vad_capture_page_current()) reason = CAPTURE_PAGE_CHANGED;
            else if (forced) reason = CAPTURE_GRANT_TIMEOUT;
            else if (stop == AUTO_STOP_AUDIO_ERROR) reason = CAPTURE_AUDIO_ERROR;
            if (mode == CAPTURE_AUTO_WAITING) {
                ESP_LOGW(TAG, "VAD start ended: id=%u reason=%s wait_ms=%u attempts=%u", frame->capture_id,
                         capture_end_reason_name(reason), (unsigned)(now_ms - s_vad_started_ms), s_vad_start_attempts);
            }
            finish_capture_locked(reason);
            trim_silence = stop == AUTO_STOP_SILENCE && !forced && mode == CAPTURE_AUTO;
        }
    }
    xSemaphoreGive(s_capture_lock);
    return trim_silence;
}

static void note_mic_error(int rc)
{
    atomic_fetch_add_explicit(&s_mic_errors, 1, memory_order_relaxed);
    atomic_store_explicit(&s_mic_last_error, rc, memory_order_relaxed);
}

static void update_mic_level(const int16_t *samples)
{
    uint64_t squares = 0;
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
        const int32_t sample = samples[i];
        squares += sample * sample;
    }
    const float mean_square = (float)squares / (FRAME_SAMPLES * 32768.0f * 32768.0f);
    const int level = mean_square > 0.0f ? (int)(1000.0f * log10f(mean_square)) : -10000;
    const int previous = atomic_load_explicit(&s_mic_level_centidb, memory_order_relaxed);
    atomic_store_explicit(&s_mic_level_centidb,
                          level > previous ? level : previous + (level - previous) / 8,
                          memory_order_relaxed);
}

/* The microphone never waits for the transport. Drop if the ring fills. */
static void mic_task(void *arg)
{
    if (!wait_for_audio_startup(arg)) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    mic_frame_t *const frame = s_mic_frame;
    static int16_t vad_mono[FRAME_SAMPLES];
    auto_capture_t automatic = {
        .detector = vad_create(VAD_MODE_2),
        .buffer = heap_caps_malloc(VAD_BUFFER_FRAMES * sizeof(vad_buffered_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    const bool vad_available = automatic.detector != NULL && automatic.buffer != NULL;
    if (!vad_available) {
        if (automatic.detector != NULL) vad_destroy(automatic.detector);
        heap_caps_free(automatic.buffer);
        ESP_LOGE(TAG, "automatic capture unavailable: VAD allocation failed");
    } else {
        ESP_LOGI(TAG, "automatic capture: onset_ms=%d preroll_ms=%d speaker_guard_ms=%d",
                 VAD_ONSET_FRAMES * FRAME_MS, VAD_PREROLL_FRAMES * FRAME_MS, VAD_SPEAKER_GUARD_MS);
    }
    s_vad_available = vad_available;
    mic_capture_progress_t progress = {0};
    unsigned release_marker_capture = 0;
    unsigned page_epoch = 0;
    unsigned peer_epoch = 0;
    unsigned warmup = VAD_RX_WARMUP_FRAMES;
    unsigned consecutive_failures = 0;
    uint32_t last_read_ms = 0;
    uint32_t read_gap_peak_ms = 0;
    bool reading = false;

    while (1) {
        frame->epoch = atomic_load(&s_capture_epoch);
        frame->capture_id = atomic_load(&s_capture_id);
        frame->page_epoch = s_voice_page_epoch;
        bool recording = s_talking;
        const bool releasing = s_release_pending;
        const bool page_open = vad_available && s_voice_page_open;
        enum capture_mode mode = s_capture_mode;
        const uint32_t now_ms = voice_now_ms();
        if (frame->epoch != peer_epoch || (!recording && !releasing && frame->page_epoch != page_epoch)) {
            clear_auto_audio(&automatic);
            warmup = VAD_RX_WARMUP_FRAMES;
            consecutive_failures = 0;
        }
        peer_epoch = frame->epoch;
        page_epoch = frame->page_epoch;

        bool auto_stopping = recording && mode != CAPTURE_MANUAL && auto_capture_should_stop(mode, now_ms);
        if (auto_stopping) {
            finish_auto_capture(frame, AUTO_STOP_CHECK_STATE);
            if (mode == CAPTURE_AUTO_WAITING && s_capture_mode == CAPTURE_AUTO) {
                mode = CAPTURE_AUTO;
                auto_stopping = auto_capture_should_stop(mode, now_ms);
            }
        }

        const bool monitoring = atomic_load(&s_monitor_enabled);
        const bool reading_now = !releasing && !auto_stopping &&
            (monitoring || (voice_link_up() && (recording || page_open)));
        int rc = ESP_CODEC_DEV_OK;
        if (reading_now != reading) {
            rc = board_audio_set_capture_active(reading_now);
            if (rc == ESP_CODEC_DEV_OK) {
                reading = reading_now;
                if (reading) {
                    last_read_ms = 0;
                    read_gap_peak_ms = 0;
                    warmup = VAD_RX_WARMUP_FRAMES;
                    atomic_store_explicit(&s_mic_gap_ms, 0, memory_order_relaxed);
                }
            }
        }
        if (!reading_now) {
            if (rc != ESP_CODEC_DEV_OK) note_mic_error(rc);
            consecutive_failures = 0;
            s_vad_listening = false;
            warmup = VAD_RX_WARMUP_FRAMES;
            if (releasing && frame->capture_id != release_marker_capture && voice_link_up() && mic_capture_current(frame)) {
                if (mode == CAPTURE_AUTO && automatic.count != 0) {
                    send_auto_audio(&automatic, frame, &progress);
                    vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
                    continue;
                }
                const uint32_t dma_overflows = board_audio_rx_overflows() - s_capture_dma_start;
                frame->type = MIC_CAPTURE_END;
                frame->delivery = (capture_delivery_t){
                    .offered_frames = progress.id == frame->capture_id ? progress.offered_frames : 0,
                    .dma_overflows = (uint16_t)(dma_overflows > UINT16_MAX ? UINT16_MAX : dma_overflows),
                };
                if (xRingbufferSend(s_up_ring, frame, MIC_CONTROL_SIZE, 0) == pdTRUE) {
                    release_marker_capture = frame->capture_id;
                }
            }
            if (!auto_stopping) clear_auto_audio(&automatic);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (rc == ESP_CODEC_DEV_OK) {
            rc = board_audio_read(frame->pcm24, vad_mono, &frame->pcm_epoch);
        }
        if (rc != ESP_CODEC_DEV_OK) {
            note_mic_error(rc);
            s_vad_listening = false;
            automatic.silent_frames = 0;
            if (!recording) clear_auto_audio(&automatic);
            if (consecutive_failures < MIC_READ_FAILURE_LIMIT) ++consecutive_failures;
            if (recording && mode != CAPTURE_MANUAL && consecutive_failures == MIC_READ_FAILURE_LIMIT) {
                finish_auto_capture(frame, AUTO_STOP_AUDIO_ERROR);
            }
            if (reading) {
                const int pause_rc = board_audio_set_capture_active(false);
                if (pause_rc == ESP_CODEC_DEV_OK) reading = false;
                else note_mic_error(pause_rc);
            }
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
            continue;
        }
        consecutive_failures = 0;
        if (monitoring || recording) update_mic_level(vad_mono);
        const uint32_t read_ms = voice_now_ms();
        if (last_read_ms != 0 && read_ms - last_read_ms > read_gap_peak_ms) {
            read_gap_peak_ms = read_ms - last_read_ms;
            atomic_store_explicit(&s_mic_gap_ms, read_gap_peak_ms, memory_order_relaxed);
        }
        last_read_ms = read_ms;
        frame->sample_started_ms = read_ms - FRAME_MS;
        if (!recording && s_talking && s_capture_mode != CAPTURE_MANUAL &&
            frame->epoch == s_capture_epoch && frame->page_epoch == s_vad_capture_page_epoch) {
            /* The transport accepted this onset while I2S was reading. */
            frame->capture_id = s_capture_id;
            mode = s_capture_mode;
            recording = true;
        }
        if (recording) {
            s_vad_listening = false;
            if (!mic_capture_current(frame)) continue;
            if (mode == CAPTURE_MANUAL) {
                clear_auto_audio(&automatic);
                queue_mic_audio(frame, &progress, MIC_DROP_IF_FULL);
                continue;
            }
            const bool speech = vad_process(automatic.detector, vad_mono, VOICE_RATE, FRAME_MS) == VAD_SPEECH;
            automatic.silent_frames = speech ? 0 : automatic.silent_frames + 1;
            buffer_auto_audio(&automatic, frame, VAD_BUFFER_FRAMES);
            if (mode == CAPTURE_AUTO) {
                send_auto_audio(&automatic, frame, &progress);
                if (automatic.silent_frames >= VAD_SILENCE_FRAMES && finish_auto_capture(frame, AUTO_STOP_SILENCE)) {
                    /* Only the newest, already classified quiet frames can be omitted. */
                    const unsigned excess = automatic.silent_frames - VAD_SILENCE_KEEP_FRAMES;
                    automatic.count -= excess < automatic.count ? excess : automatic.count;
                }
            }
            continue;
        }

        if (warmup > 0) warmup--;
        const uint32_t sampled_ms = voice_now_ms();
        const bool can_buffer = page_open && voice_link_up() && warmup == 0 && vad_speaker_quiet() &&
            frame->page_epoch == s_voice_page_epoch && frame->epoch == s_capture_epoch && s_voice_page_open;
        const bool host_ready = vad_host_ready(sampled_ms);
        s_vad_listening = can_buffer && host_ready;
        if (!can_buffer) {
            if (!s_talking) clear_auto_audio(&automatic);
            continue;
        }
        const bool start_requested = automatic.request_ms != 0 && sampled_ms - automatic.request_ms < VAD_START_QUEUE_MS;
        if (!start_requested && automatic.onset_ms != 0 && sampled_ms - automatic.onset_ms >= VAD_ONSET_HOLD_MS) {
            automatic.onset_ms = 0;
            automatic.voiced_frames = 0;
        }
        const bool speech = vad_process(automatic.detector, vad_mono, VOICE_RATE, FRAME_MS) == VAD_SPEECH;
        if (automatic.onset_ms == 0) {
            automatic.voiced_frames = speech ? automatic.voiced_frames + 1 : 0;
            if (automatic.voiced_frames >= VAD_ONSET_FRAMES) automatic.onset_ms = sampled_ms;
        }
        automatic.silent_frames = speech ? 0 : automatic.silent_frames + 1;
        /* Keep a detected onset through brief host busy periods. */
        buffer_auto_audio(&automatic, frame,
                          automatic.onset_ms != 0 || start_requested ? VAD_BUFFER_FRAMES : VAD_PREROLL_FRAMES);
        if (host_ready && automatic.onset_ms != 0 && !start_requested) {
            frame->type = MIC_AUTO_START;
            frame->onset.buffered_frames = automatic.count;
            frame->onset.held_ms = (uint16_t)(sampled_ms - automatic.onset_ms);
            if (xRingbufferSend(s_up_ring, frame, MIC_CONTROL_SIZE, 0) == pdTRUE) automatic.request_ms = sampled_ms;
        }
    }
}

static void send_auto_start(const mic_frame_t *frame)
{
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const uint32_t now_ms = voice_now_ms();
    if (s_voice_page_open && frame->page_epoch == s_voice_page_epoch && frame->epoch == s_capture_epoch &&
        frame->capture_id == s_capture_id && !s_talking && !s_release_pending && voice_link_up() &&
        now_ms - (frame->sample_started_ms + FRAME_MS) < VAD_START_QUEUE_MS &&
        vad_host_ready(now_ms) && vad_speaker_quiet()) {
        const bool wifi = !atomic_load(&s_ble_link_up) && atomic_load(&s_link_up);
        // Wi-Fi also publishes this state on every backend ping and on toggle.
        if (send_tts_state() || wifi) {
            int udp_error;
            const bool queued = send_vad_capture_control("vad_start", frame->capture_id + 1, &udp_error);
            // Keep the onset for an early retry after a failed local Wi-Fi send.
            if (queued || (wifi && atomic_load(&s_link_up))) {
                s_vad_started_ms = voice_now_ms();
                s_vad_start_retry_ms = s_vad_started_ms + vad_start_retry_interval_ms();
                s_vad_start_attempts = 1;
                s_vad_capture_page_epoch = frame->page_epoch;
                start_capture_locked(CAPTURE_AUTO_WAITING);
                ESP_LOGI(TAG, "-> vad_start: id=%u buffered_ms=%u held_ms=%u grant_timeout_ms=%u queued=%d udp_errno=%d",
                         (unsigned)s_capture_id, (unsigned)frame->onset.buffered_frames * FRAME_MS,
                         (unsigned)frame->onset.held_ms, (unsigned)vad_grant_timeout_ms(), queued, udp_error);
            }
        }
    }
    xSemaphoreGive(s_capture_lock);
}

static void retry_vad_capture_start(void)
{
    if (!s_talking || s_capture_mode != CAPTURE_AUTO_WAITING) return;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const uint32_t now_ms = voice_now_ms();
    if (s_talking && s_capture_mode == CAPTURE_AUTO_WAITING && voice_link_up() &&
        !auto_capture_should_stop(CAPTURE_AUTO_WAITING, now_ms) &&
        (int32_t)(now_ms - s_vad_start_retry_ms) >= 0) {
        s_vad_start_retry_ms = now_ms + vad_start_retry_interval_ms();
        ++s_vad_start_attempts;
        const unsigned id = s_capture_id;
        int udp_error;
        const bool queued = send_vad_capture_control("vad_start", id, &udp_error);
        ESP_LOGI(TAG, "VAD start retry: id=%u attempt=%u wait_ms=%u queued=%d udp_errno=%d", id,
                 s_vad_start_attempts, (unsigned)(now_ms - s_vad_started_ms), queued, udp_error);
    }
    xSemaphoreGive(s_capture_lock);
}

typedef struct {
    unsigned sent;
    unsigned encode_errors;
    unsigned send_errors;
    unsigned udp_pressure;
    unsigned udp_other;
    unsigned udp_retries;
    unsigned udp_recovered;
    int last_udp_error;
} capture_tx_stats_t;

static bool note_capture_udp_error(capture_tx_stats_t *stats, int error)
{
    if (error == 0) return false;
    stats->last_udp_error = error;
    if (udp_send_has_pressure(error)) {
        ++stats->udp_pressure;
        return true;
    }
    ++stats->udp_other;
    return false;
}

static esp_err_t send_uplink_frame(const uint8_t *packet, uint16_t length, const mic_frame_t *frame,
                                   int64_t work_started, capture_tx_stats_t *stats)
{
    int udp_error;
    const esp_err_t sent = send_frame(T_AUDIO_UP, CAPTURE_DELIVERY_FLAG, packet, length, frame, &udp_error, 0);
    if (!note_capture_udp_error(stats, udp_error)) return sent;

    const int64_t failed_at = esp_timer_get_time();
    int64_t retry_deadline = failed_at + WIFI_AUDIO_RETRY_WINDOW_US;
    // Leave time for the synchronous lwIP send itself as well as the retry delay.
    const int64_t budget_deadline = work_started + FRAME_MS * 1000 - WIFI_AUDIO_RETRY_WINDOW_US;
    if (retry_deadline > budget_deadline) retry_deadline = budget_deadline;
    if (retry_deadline - failed_at <= 1000 || !mic_capture_current(frame) ||
        !atomic_load(&s_link_up) || atomic_load(&s_ble_link_up)) return sent;

    UBaseType_t pending = 0;
    vRingbufferGetInfo(s_up_ring, NULL, NULL, NULL, NULL, &pending);
    if (pending > 1) return sent;
    vTaskDelay(pdMS_TO_TICKS(1));
    vRingbufferGetInfo(s_up_ring, NULL, NULL, NULL, NULL, &pending);
    if (pending > 1 || esp_timer_get_time() >= retry_deadline) return sent;

    const esp_err_t retry = send_frame(T_AUDIO_UP, CAPTURE_DELIVERY_FLAG, packet, length, frame, &udp_error,
                                       retry_deadline);
    if (retry == ESP_OK || udp_error != 0) ++stats->udp_retries;
    note_capture_udp_error(stats, udp_error);
    if (retry != ESP_OK) return sent;
    ++stats->udp_recovered;
    return ESP_OK;
}

/* I2S supplies one 20 ms frame at a time. Wake on the queue to avoid adding
 * a second clock that can miss a frame while filtering or encoding. */
static void *tx_task(void *arg)
{
    if (!wait_for_audio_startup(arg)) return NULL;
    static uint8_t packet[OPUS_MAX_PACKET];
    unsigned capture_id = 0;
    uint32_t pcm_epoch = 0;
    uint16_t next_sequence = 0;
    uint32_t queue_peak_us = 0;
    unsigned encode_peak_us = 0;
    unsigned lock_peak_us = 0;
    unsigned packet_peak = 0;
    unsigned work_peak_us = 0;
    capture_tx_stats_t capture_tx = {0};

    while (1) {
        size_t size = 0;
        mic_frame_t *frame = xRingbufferReceive(s_up_ring, &size, portMAX_DELAY);
        if (frame == NULL) {
            continue;
        }
        if (size < MIC_CONTROL_SIZE || size !=
            (frame->type == MIC_AUTO_START || frame->type == MIC_CAPTURE_END ? MIC_CONTROL_SIZE : sizeof(*frame))) {
            vRingbufferReturnItem(s_up_ring, frame);
            continue;
        }
        if (frame->type == MIC_AUTO_START) {
            send_auto_start(frame);
            vRingbufferReturnItem(s_up_ring, frame);
            continue;
        }
        if (!mic_capture_current(frame)) {
            vRingbufferReturnItem(s_up_ring, frame);
            continue;
        }

        if (frame->type == MIC_CAPTURE_END) {
            xSemaphoreTake(s_capture_lock, portMAX_DELAY);
            const bool release = s_release_pending && mic_capture_current(frame);
            const bool ble = atomic_load(&s_ble_link_up);
            const bool automatic = s_capture_mode != CAPTURE_MANUAL;
            const enum capture_end_reason reason = s_capture_end_reason;
            if (release) {
                s_release_ready = true;
                s_release_delivery_sent = false;
                s_release_delivery = frame->delivery;
                s_release_retry_ms = 0;
                if (s_capture_mode == CAPTURE_MANUAL) {
                    atomic_store_explicit(&s_pending_beep, SPEAKER_BEEP_END, memory_order_relaxed);
                }
                send_capture_release_locked(esp_timer_get_time() / 1000);
            }
            xSemaphoreGive(s_capture_lock);
            if (release) {
                const capture_tx_stats_t stats = capture_id == frame->capture_id ? capture_tx : (capture_tx_stats_t){0};
                const bool zero_frame_vad = automatic && frame->delivery.offered_frames == 0;
                ESP_LOG_LEVEL(zero_frame_vad ? ESP_LOG_WARN : ESP_LOG_INFO, TAG,
                              "capture uplink: transport=%s id=%u offered=%u sent=%u encode_err=%u send_err=%u "
                              "udp_pressure=%u udp_other=%u udp_errno=%d dma_ovf=%u end_reason=%s zero_frame_vad=%d "
                              "udp_retries=%u udp_recovered=%u",
                              ble ? "ble" : "udp", frame->capture_id, (unsigned)frame->delivery.offered_frames,
                              stats.sent, stats.encode_errors, stats.send_errors, stats.udp_pressure,
                              stats.udp_other, stats.last_udp_error, (unsigned)frame->delivery.dma_overflows,
                              capture_end_reason_name(reason), zero_frame_vad, stats.udp_retries, stats.udp_recovered);
            }
            vRingbufferReturnItem(s_up_ring, frame);
            continue;
        }

        const bool new_capture = frame->capture_id != capture_id;
        if (new_capture) {
            capture_id = frame->capture_id;
            queue_peak_us = 0;
            encode_peak_us = 0;
            lock_peak_us = 0;
            packet_peak = 0;
            work_peak_us = 0;
            capture_tx = (capture_tx_stats_t){0};
            atomic_store_explicit(&s_up_packet_peak, 0, memory_order_relaxed);
        }
        const int64_t work_started = esp_timer_get_time();
        const uint32_t wait_us = (uint32_t)work_started - frame->queued_us;
        if (wait_us > queue_peak_us) queue_peak_us = wait_us;
        atomic_store_explicit(&s_up_queue_peak_us, queue_peak_us, memory_order_relaxed);

        if (new_capture || pcm_epoch != frame->pcm_epoch || next_sequence != frame->audio_sequence) {
            stereo_wiener_48k_reset(&s_uplink_dsp->wiener);
        }
        pcm_epoch = frame->pcm_epoch;
        next_sequence = frame->audio_sequence + 1;
        stereo_wiener_48k_process(&s_uplink_dsp->wiener, frame->pcm24);
        const int64_t filtered = esp_timer_get_time();
        atomic_store_explicit(&s_wiener_us, (unsigned)(filtered - work_started), memory_order_relaxed);
        board_audio_unpack_capture(frame->pcm24, s_uplink_dsp->opus_pcm, UPLINK_SAMPLES);

        const int64_t lock_started = esp_timer_get_time();
        xSemaphoreTake(s_opus_lock, portMAX_DELAY);
        const int64_t started = esp_timer_get_time();
        const int n = opus_encode(s_enc, s_uplink_dsp->opus_pcm, UPLINK_SAMPLES, packet, sizeof(packet));
        const int64_t encoded = esp_timer_get_time();
        xSemaphoreGive(s_opus_lock);
        const unsigned encode_us = (unsigned)(encoded - started);
        const unsigned work_us = (unsigned)(encoded - work_started);
        if (work_us > work_peak_us) work_peak_us = work_us;
        atomic_store_explicit(&s_uplink_work_peak_us, work_peak_us, memory_order_relaxed);
        if (work_us > FRAME_MS * 1000) atomic_fetch_add_explicit(&s_uplink_work_late, 1, memory_order_relaxed);
        const unsigned lock_us = (unsigned)(started - lock_started);
        if (lock_us > lock_peak_us) lock_peak_us = lock_us;
        atomic_store_explicit(&s_opus_lock_peak_us, lock_peak_us, memory_order_relaxed);
        atomic_store_explicit(&s_encode_us, encode_us, memory_order_relaxed);
        if (encode_us > encode_peak_us) encode_peak_us = encode_us;
        atomic_store_explicit(&s_encode_peak_us, encode_peak_us, memory_order_relaxed);
        if (encode_us > FRAME_MS * 1000) atomic_fetch_add_explicit(&s_encode_late, 1, memory_order_relaxed);
        unsigned send_us = 0;
        if (n <= 0) {
            ++capture_tx.encode_errors;
            atomic_fetch_add_explicit(&s_encode_errors, 1, memory_order_relaxed);
        } else {
            const esp_err_t sent = send_uplink_frame(packet, (uint16_t)n, frame, work_started, &capture_tx);
            if (sent == ESP_OK) {
                send_us = (unsigned)(esp_timer_get_time() - work_started);
                ++capture_tx.sent;
                atomic_fetch_add_explicit(&s_up_frames, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&s_up_bytes, (uint32_t)n, memory_order_relaxed);
                if (opus_packet_get_bandwidth(packet) >= OPUS_BANDWIDTH_SUPERWIDEBAND) {
                    atomic_fetch_add_explicit(&s_up_swb_fb_frames, 1, memory_order_relaxed);
                }
                if ((unsigned)n > packet_peak) {
                    packet_peak = (unsigned)n;
                    atomic_store_explicit(&s_up_packet_peak, packet_peak, memory_order_relaxed);
                }
            } else if (sent != ESP_ERR_INVALID_STATE) {
                ++capture_tx.send_errors;
                atomic_fetch_add_explicit(&s_send_errors, 1, memory_order_relaxed);
            }
        }

        atomic_store_explicit(&s_uplink_send_us, send_us, memory_order_relaxed);
        vRingbufferReturnItem(s_up_ring, frame);
    }
    return NULL;
}

typedef struct {
    unsigned epoch;
    bool playing;
    int64_t primed_since_us;
    int64_t last_played_us;
} playback_buffer_t;

static void *next_playback_packet(playback_buffer_t *buffer, int64_t now_us, size_t *length)
{
    const unsigned epoch = atomic_load(&s_down_epoch);
    if (buffer->epoch != epoch) {
        *buffer = (playback_buffer_t){.epoch = epoch};
    }
    if (!atomic_load(&s_tts_enabled) || atomic_load(&s_talking) || atomic_load(&s_release_pending)) {
        return NULL;
    }

    if (buffer->playing) {
        void *packet = xRingbufferReceive(s_down_ring, length, 0);
        if (packet != NULL) return packet;
        buffer->playing = false;
    }

    UBaseType_t queued = 0;
    vRingbufferGetInfo(s_down_ring, NULL, NULL, NULL, NULL, &queued);
    if (queued == 0) {
        buffer->primed_since_us = 0;
        return NULL;
    }
    if (buffer->primed_since_us == 0) buffer->primed_since_us = now_us;
    if (queued < PLAYBACK_PREFILL_FRAMES && now_us - buffer->primed_since_us < PLAYBACK_PREFILL_US) {
        return NULL;
    }

    buffer->primed_since_us = 0;
    void *packet = xRingbufferReceive(s_down_ring, length, 0);
    if (packet != NULL) {
        buffer->playing = true;
        /* Count only when audio resumes. A normal end of speech is idle. */
        if (buffer->last_played_us != 0 && now_us - buffer->last_played_us < PLAYBACK_RESUME_US) {
            atomic_fetch_add_explicit(&s_down_rebuffers, 1, memory_order_relaxed);
        }
    }
    return packet;
}

/* I2S DMA paces output. The speaker never waits for the transport queue. */
static void *spk_task(void *arg)
{
    if (!wait_for_audio_startup(arg)) return NULL;
    static int16_t mono[FRAME_SAMPLES];
    playback_buffer_t buffer = {0};
    const int16_t *beep = NULL;
    int beep_length = 0;
    int beep_pos = 0;

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        size_t size = 0;
        void *packet = next_playback_packet(&buffer, now_us, &size);
        int samples = 0;
        if (packet != NULL) {
            if (xSemaphoreTake(s_opus_lock, 0) == pdTRUE) {
                samples = opus_decode(s_dec, packet, size, mono, FRAME_SAMPLES, 0);
                xSemaphoreGive(s_opus_lock);
            } else if (buffer.epoch == atomic_load(&s_down_epoch) && !atomic_load(&s_talking)) {
                atomic_fetch_add_explicit(&s_decode_errors, 1, memory_order_relaxed);
            }
            vRingbufferReturnItem(s_down_ring, packet);
            if (samples < 0) {
                atomic_fetch_add_explicit(&s_decode_errors, 1, memory_order_relaxed);
                samples = 0;
            } else if (buffer.epoch != atomic_load(&s_down_epoch) || atomic_load(&s_talking)) {
                samples = 0;
            } else if (samples > 0) {
                atomic_fetch_add_explicit(&s_down_frames, 1, memory_order_relaxed);
                buffer.last_played_us = now_us;
                s_readback_active_ms = (uint32_t)(now_us / 1000);
            }
        }
        if (samples == 0) atomic_fetch_add_explicit(&s_down_idle, 1, memory_order_relaxed);

        const int pending_beep = atomic_exchange_explicit(&s_pending_beep, SPEAKER_BEEP_NONE, memory_order_relaxed);
        if (pending_beep != SPEAKER_BEEP_NONE) {
            const bool ending = pending_beep == SPEAKER_BEEP_END;
            beep = ending ? s_beep_down : s_beep_up;
            beep_length = ending ? s_beep_down_len : s_beep_up_len;
            beep_pos = 0;
        }
        /* Keep I2S paced during idle without mixing or interpolating silence. */
        const bool active = samples > 0 || beep_pos < beep_length;
        if (active) {
            s_speaker_active_ms = (uint32_t)(now_us / 1000);
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                int16_t v = i < samples ? mono[i] : 0;
                if (beep_pos < beep_length) {
                    const int mixed = v + beep[beep_pos++];
                    v = (int16_t)(mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
                }
                mono[i] = v;
            }
        }
        if (board_audio_write(active ? mono : NULL) != ESP_CODEC_DEV_OK) {
            atomic_fetch_add_explicit(&s_speaker_errors, 1, memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        }
    }
    return NULL;
}

/* --------------------------------------------------------------- telemetry */

/* One line every five seconds: audio and link diagnostics from a serial capture. */

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t up_frames = 0, up_bytes = 0, up_swb_fb_frames = 0, wifi_catchup_frames = 0;
    uint32_t down_frames = 0, down_rebuffers = 0, down_idle = 0;
    uint32_t down_dropped = 0, speaker_errors = 0, encode_late = 0, work_late = 0;
    ble_voice_audio_drops_t ble_drops = {0};
    uint32_t mic_frames = 0, mic_errors = 0, mic_dropped = 0, rx_dma_overflows = 0;
    uint32_t enc_err = 0, send_err = 0, dec_err = 0;
    int64_t window_start = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        const uint32_t cur_up = atomic_load_explicit(&s_up_frames, memory_order_relaxed);
        const uint32_t cur_bytes = atomic_load_explicit(&s_up_bytes, memory_order_relaxed);
        const uint32_t cur_swb_fb = atomic_load_explicit(&s_up_swb_fb_frames, memory_order_relaxed);
        const uint32_t cur_wifi_catchup = atomic_load_explicit(&s_wifi_catchup_frames, memory_order_relaxed);
        const uint32_t cur_down = atomic_load_explicit(&s_down_frames, memory_order_relaxed);
        const uint32_t cur_rebuffers = atomic_load_explicit(&s_down_rebuffers, memory_order_relaxed);
        const uint32_t cur_idle = atomic_load_explicit(&s_down_idle, memory_order_relaxed);
        const uint32_t cur_down_drop = atomic_load_explicit(&s_down_dropped, memory_order_relaxed);
        const uint32_t cur_speaker_errors = atomic_load_explicit(&s_speaker_errors, memory_order_relaxed);
        const uint32_t cur_encode_late = atomic_load_explicit(&s_encode_late, memory_order_relaxed);
        const uint32_t cur_work_late = atomic_load_explicit(&s_uplink_work_late, memory_order_relaxed);
        const ble_voice_audio_drops_t cur_ble_drops = ble_voice_audio_drops();
        const uint32_t cur_mic = atomic_load_explicit(&s_mic_frames, memory_order_relaxed);
        const uint32_t cur_mic_err = atomic_load_explicit(&s_mic_errors, memory_order_relaxed);
        const uint32_t cur_mic_drop = atomic_load_explicit(&s_mic_dropped, memory_order_relaxed);
        const uint32_t cur_rx_dma_overflows = board_audio_rx_overflows();
        const uint32_t cur_enc = atomic_load_explicit(&s_encode_errors, memory_order_relaxed);
        const uint32_t cur_send = atomic_load_explicit(&s_send_errors, memory_order_relaxed);
        const uint32_t cur_dec = atomic_load_explicit(&s_decode_errors, memory_order_relaxed);
        const int64_t now = esp_timer_get_time();
        const double secs = (now - window_start) / 1e6;
        if (secs <= 0.0) {
            continue;
        }

        int rssi = 0, channel = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
            channel = ap.primary;
        }

        ESP_LOGI(TAG,
                 "telemetry: mic=%.1ffps mic_ring_drop=%u rx_dma_ovf=%u read_gap_peak=%ums err=%u rc=%d | "
                 "up=%.1ffps/%.1fkbps swb_fb=%u packet_peak=%uB queue_peak=%.2fms lock_peak=%.2fms "
                 "wiener_last=%.2fms encode_last=%.2fms encode_peak=%.2fms encode_late=%u "
                 "work_peak=%.2fms work_late=%u wifi_catchup=%u | "
                 "down=%.1ffps rebuffer=%u down_drop=%u idle=%u spk_err=%u dec_err=%u enc_err=%u send_err=%u | "
                 "ble_tx_drop=%u ble_rx_drop=%u ble_tx_admission_drop=%u ble_tx_retry=%u | "
                 "heap=%uK min=%uK dma_max=%uB psram=%uK | rssi=%d ch=%d vol=%d up=%llds",
                 (cur_mic - mic_frames) / secs, (unsigned)(cur_mic_drop - mic_dropped),
                 (unsigned)(cur_rx_dma_overflows - rx_dma_overflows),
                 atomic_load_explicit(&s_mic_gap_ms, memory_order_relaxed),
                 (unsigned)(cur_mic_err - mic_errors), atomic_load_explicit(&s_mic_last_error, memory_order_relaxed),
                 (cur_up - up_frames) / secs, (cur_bytes - up_bytes) * 8.0 / 1000.0 / secs,
                 (unsigned)(cur_swb_fb - up_swb_fb_frames), atomic_load_explicit(&s_up_packet_peak, memory_order_relaxed),
                 atomic_load_explicit(&s_up_queue_peak_us, memory_order_relaxed) / 1000.0,
                 atomic_load_explicit(&s_opus_lock_peak_us, memory_order_relaxed) / 1000.0,
                 atomic_load_explicit(&s_wiener_us, memory_order_relaxed) / 1000.0,
                 atomic_load_explicit(&s_encode_us, memory_order_relaxed) / 1000.0,
                 atomic_load_explicit(&s_encode_peak_us, memory_order_relaxed) / 1000.0,
                 (unsigned)(cur_encode_late - encode_late),
                 atomic_load_explicit(&s_uplink_work_peak_us, memory_order_relaxed) / 1000.0,
                 (unsigned)(cur_work_late - work_late),
                 (unsigned)(cur_wifi_catchup - wifi_catchup_frames),
                 (cur_down - down_frames) / secs, (unsigned)(cur_rebuffers - down_rebuffers),
                 (unsigned)(cur_down_drop - down_dropped), (unsigned)(cur_idle - down_idle),
                 (unsigned)(cur_speaker_errors - speaker_errors),
                 (unsigned)(cur_dec - dec_err), (unsigned)(cur_enc - enc_err), (unsigned)(cur_send - send_err),
                 (unsigned)(cur_ble_drops.tx - ble_drops.tx), (unsigned)(cur_ble_drops.rx - ble_drops.rx),
                 (unsigned)(cur_ble_drops.tx_admission_drops - ble_drops.tx_admission_drops),
                 (unsigned)(cur_ble_drops.tx_retries - ble_drops.tx_retries),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 rssi, channel, s_volume,
                 (long long)(now / 1000000));

        up_frames = cur_up; up_bytes = cur_bytes; up_swb_fb_frames = cur_swb_fb;
        wifi_catchup_frames = cur_wifi_catchup;
        down_frames = cur_down; down_rebuffers = cur_rebuffers; down_idle = cur_idle;
        down_dropped = cur_down_drop; speaker_errors = cur_speaker_errors;
        encode_late = cur_encode_late;
        work_late = cur_work_late;
        ble_drops = cur_ble_drops;
        mic_frames = cur_mic; mic_errors = cur_mic_err; mic_dropped = cur_mic_drop;
        rx_dma_overflows = cur_rx_dma_overflows;
        enc_err = cur_enc; send_err = cur_send; dec_err = cur_dec;
        window_start = now;
    }
}


void local_voice_set_volume(int value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (!s_audio_ready) return;

    xSemaphoreTake(s_volume_lock, portMAX_DELAY);
    if (s_volume != value) {
        const int rc = esp_codec_dev_set_out_vol(s_spk, value);
        if (rc == ESP_CODEC_DEV_OK) {
            s_volume = value;
        } else {
            ESP_LOGW(TAG, "speaker volume rc=%d", rc);
        }
    }
    xSemaphoreGive(s_volume_lock);
}

void local_voice_set_gain(int gain_db)
{
    if (gain_db < 0 || gain_db > 36 || !s_audio_ready) return;
    const float requested = gain_db == 33 ? 34.5f : (float)gain_db;
    const uint16_t channels = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    xSemaphoreTake(s_volume_lock, portMAX_DELAY);
    const int rc = esp_codec_dev_set_in_channel_gain(s_mic, channels, requested);
    xSemaphoreGive(s_volume_lock);
    if (rc != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "microphone gain rc=%d", rc);
}

void local_voice_set_listening(bool enabled)
{
    if (atomic_exchange(&s_voice_page_open, enabled) != enabled) {
        atomic_fetch_add(&s_voice_page_epoch, 1);
        s_vad_ready_until_ms = 0;
        s_vad_host_busy = false;
        s_vad_listening = false;
        atomic_store(&s_voice_emotion, LOCAL_VOICE_EMOTION_NONE);
    }
}

void local_voice_set_tts(bool enabled)
{
    if (atomic_exchange(&s_tts_enabled, enabled) == enabled) return;
    if (!enabled && s_audio_ready) reset_downlink_audio();
    send_tts_state();
}

bool local_voice_connected(void)
{
    return voice_link_up();
}

bool local_voice_recording(void)
{
    return atomic_load(&s_talking);
}

bool local_voice_listening(void)
{
    return atomic_load(&s_vad_listening);
}

bool local_voice_processing(void)
{
    return s_release_pending || (s_voice_page_open && s_vad_host_busy);
}

bool local_voice_speaking(void)
{
    const uint32_t readback_ms = s_readback_active_ms;
    return s_tts_enabled && readback_ms != 0 && voice_now_ms() - readback_ms < 150;
}

local_voice_emotion_t local_voice_emotion(void)
{
    const uint32_t emotion_ms = s_voice_emotion_ms;
    if (voice_now_ms() - emotion_ms >= VOICE_EMOTION_HOLD_MS) return LOCAL_VOICE_EMOTION_NONE;
    return (local_voice_emotion_t)atomic_load(&s_voice_emotion);
}

void local_voice_set_monitor(bool enabled)
{
    atomic_store(&s_monitor_enabled, enabled);
    if (!enabled) atomic_store(&s_mic_level_centidb, -10000);
}

float local_voice_monitor_db(void)
{
    return (s_monitor_enabled || s_talking) ? (float)atomic_load(&s_mic_level_centidb) / 100.0f : -100.0f;
}

void local_voice_request_chirp(void)
{
    if (s_audio_ready && !s_talking && !s_release_pending)
        atomic_store_explicit(&s_pending_beep, SPEAKER_BEEP_START, memory_order_relaxed);
}

bool local_voice_pair(bool replace)
{
    return ble_voice_open_pairing(replace);
}

bool local_voice_repair(void)
{
    return ble_voice_repair_pairing();
}

bool local_voice_retry(void)
{
    return ble_voice_retry_connection();
}

bool local_voice_paired(void)
{
    return ble_voice_has_saved_peer();
}

bool local_voice_pairing(void)
{
    return ble_voice_is_pairing();
}

bool local_voice_ble_connected(void)
{
    return ble_voice_is_connected();
}

/* ------------------------------------------------------------------ network */

void local_voice_wifi_offline(void)
{
    if (atomic_exchange(&s_link_up, false) && !atomic_load(&s_ble_link_up)) reset_voice_audio();
}

static bool udp_set_receive_timeout(int sock, unsigned timeout_ms)
{
    const struct timeval timeout = {.tv_sec = 0, .tv_usec = timeout_ms * 1000};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0) return true;
    ESP_LOGW(TAG, "UDP receive timeout %u ms failed: errno=%d", timeout_ms, errno);
    return false;
}

static int udp_open(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind udp :%d failed (errno %d)", SERVER_PORT, errno);
        close(sock);
        return -1;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (!udp_set_receive_timeout(sock, UDP_IDLE_RECEIVE_MS)) {
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "udp listening on :%d", SERVER_PORT);
    return sock;
}

static void clear_audio_ring(RingbufHandle_t ring)
{
    size_t size;
    void *item;
    while ((item = xRingbufferReceive(ring, &size, 0)) != NULL) {
        vRingbufferReturnItem(ring, item);
    }
}

static void reset_voice_audio(void)
{
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    s_talking = false;
    s_release_pending = false;
    s_release_ready = false;
    s_wifi_report.deadline_ms = 0;
    s_vad_ready_until_ms = 0;
    s_vad_host_busy = false;
    s_voice_emotion_capture = false;
    atomic_store(&s_voice_emotion, LOCAL_VOICE_EMOTION_NONE);
    atomic_fetch_add(&s_capture_epoch, 1);
    clear_audio_ring(s_up_ring);
    reset_downlink_audio();
    xSemaphoreGive(s_capture_lock);
}

static void cancel_capture_locked(void)
{
    if (atomic_exchange(&s_talking, false)) atomic_fetch_add(&s_capture_epoch, 1);
    s_vad_ready_until_ms = 0;
    s_vad_ended_ms = voice_now_ms();
    s_release_pending = false;
    s_release_ready = false;
    s_voice_emotion_capture = false;
    atomic_store(&s_voice_emotion, LOCAL_VOICE_EMOTION_NONE);
}

static bool handle_vad_ack(const char *cmd)
{
    static const char start[] = "vad_capture_start:";
    static const char start_with_report[] = "vad_capture_start2:";
    static const char reject[] = "vad_capture_reject:";
    const bool accepted_report = strncmp(cmd, start_with_report, sizeof(start_with_report) - 1) == 0;
    const bool accepted = accepted_report || strncmp(cmd, start, sizeof(start) - 1) == 0;
    const char *id;
    if (accepted_report) id = cmd + sizeof(start_with_report) - 1;
    else if (accepted) id = cmd + sizeof(start) - 1;
    else if (strncmp(cmd, reject, sizeof(reject) - 1) == 0) id = cmd + sizeof(reject) - 1;
    else return false;

    const uint32_t received_ms = voice_now_ms();
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    char capture_id[16];
    snprintf(capture_id, sizeof(capture_id), "%u", (unsigned)s_capture_id);
    if (s_capture_mode == CAPTURE_AUTO_WAITING && strcmp(id, capture_id) == 0) {
        const unsigned wait_ms = (unsigned)(received_ms - s_vad_started_ms);
        if (!s_talking || auto_capture_should_stop(CAPTURE_AUTO_WAITING, received_ms)) {
            if (accepted) ESP_LOGW(TAG, "VAD start grant ignored: id=%s wait_ms=%u", capture_id, wait_ms);
        } else if (!accepted) {
            ESP_LOGI(TAG, "VAD start rejected: id=%s wait_ms=%u attempts=%u", capture_id, wait_ms, s_vad_start_attempts);
            cancel_capture_locked();
        } else {
            atomic_store(&s_vad_grant_ms, received_ms);
            s_capture_report = accepted_report && !atomic_load(&s_ble_link_up);
            s_capture_mode = CAPTURE_AUTO;
            s_voice_emotion_capture = true;
            ESP_LOGI(TAG, "VAD start granted: id=%s wait_ms=%u attempts=%u", capture_id, wait_ms, s_vad_start_attempts);
        }
    }
    xSemaphoreGive(s_capture_lock);
    return true;
}

static void apply_voice_emotion(const char *cmd)
{
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const uint32_t now = voice_now_ms();
    const uint32_t ended = s_vad_ended_ms;
    char prefix[24];
    const size_t length = (size_t)snprintf(prefix, sizeof(prefix), "emotion:%u:", (unsigned)s_capture_id);
    if (s_voice_emotion_capture && s_voice_page_open && s_voice_page_epoch == s_vad_capture_page_epoch &&
        (s_talking || s_release_pending || (ended != 0 && now - ended < VOICE_EMOTION_RESULT_MS)) &&
        strncmp(cmd, prefix, length) == 0 && cmd[length] >= '0' &&
        cmd[length] <= '0' + LOCAL_VOICE_EMOTION_ANGRY && cmd[length + 1] == '\0') {
        atomic_store(&s_voice_emotion_ms, now);
        atomic_store(&s_voice_emotion, cmd[length] - '0');
    }
    xSemaphoreGive(s_capture_lock);
}

static void handle_ctrl(const char *cmd)
{
    if (strncmp(cmd, "vad_end_ack:", 12) == 0) {
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        char expected[32];
        snprintf(expected, sizeof(expected), "vad_end_ack:%u", s_wifi_report.id);
        if (s_wifi_report.deadline_ms != 0 && strcmp(cmd, expected) == 0) {
            ESP_LOGI(TAG, "Wi-Fi capture report confirmed: id=%u offered=%u", s_wifi_report.id,
                     (unsigned)s_wifi_report.delivery.offered_frames);
            s_wifi_report.deadline_ms = 0;
        }
        xSemaphoreGive(s_capture_lock);
        return;
    }
    if (strncmp(cmd, "emotion:", 8) == 0) {
        apply_voice_emotion(cmd);
        return;
    }
    if (strcmp(cmd, "vad_ready") == 0 || strcmp(cmd, "vad_busy") == 0) {
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        const uint32_t now_ms = voice_now_ms();
        const uint32_t ended_ms = s_vad_ended_ms;
        if (s_voice_page_open && !s_talking && !s_release_pending &&
            (ended_ms == 0 || now_ms - ended_ms >= VAD_STATUS_POLL_MS)) {
            const bool ready = strcmp(cmd, "vad_ready") == 0;
            const uint32_t lease = atomic_load(&s_ble_link_up) ? VAD_BLE_STATUS_LEASE_MS : VAD_WIFI_STATUS_LEASE_MS;
            s_vad_ready_until_ms = ready ? now_ms + lease : 0;
            s_vad_host_busy = !ready;
        }
        xSemaphoreGive(s_capture_lock);
        return;
    }
    if (handle_vad_ack(cmd)) return;
    if (strncmp(cmd, "volume:", 7) == 0) {
        // The host sends its default at startup and on reconnect. Use the watch's Sound setting.
        ESP_LOGI(TAG, "kept watch speaker volume=%d (PC volume request)", atomic_load(&s_volume));
        return;
    }
    ESP_LOGI(TAG, "<- %s", cmd);
    if (strcmp(cmd, "stop_playback") == 0) {
        reset_downlink_audio();
    } else if (strcmp(cmd, "capture_start") == 0) {
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        if (s_audio_ready && !s_release_pending && !s_talking) {
            start_capture_locked(CAPTURE_MANUAL);
        }
        xSemaphoreGive(s_capture_lock);
    } else if (strcmp(cmd, "capture_stop") == 0) {
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        cancel_capture_locked();
        xSemaphoreGive(s_capture_lock);
    }
}

static bool valid_voice_packet(const uint8_t *packet, size_t size)
{
    if (size < HDR_BYTES || packet[0] != 0xA5 || packet[1] != 0xC3) {
        return false;
    }
    const size_t length = (size_t)packet[6] | ((size_t)packet[7] << 8);
    return length == size - HDR_BYTES && size <= BLE_VOICE_MAX_FRAME_BYTES;
}

static bool handle_clock_sync(const uint8_t *payload, size_t length, bool from_ble)
{
    static const char prefix[] = "clock_sync:";
    const size_t prefix_length = sizeof(prefix) - 1;
    if (length < prefix_length || memcmp(payload, prefix, prefix_length) != 0) return false;
    if (!from_ble || length != prefix_length + 13) return true;

    uint64_t timestamp_ms = 0;
    for (size_t i = prefix_length; i < length; ++i) {
        if (payload[i] < '0' || payload[i] > '9') return true;
        timestamp_ms = timestamp_ms * 10 + (payload[i] - '0');
    }
    /* Match the watch face readiness threshold and reject dates from 2100 onwards. */
    if (timestamp_ms < UINT64_C(1700000000000) || timestamp_ms >= UINT64_C(4102444800000)) return true;
    const struct timeval time = {
        .tv_sec = (time_t)(timestamp_ms / 1000),
        .tv_usec = (suseconds_t)((timestamp_ms % 1000) * 1000),
    };
    if (settimeofday(&time, NULL) == 0) {
        ESP_LOGI(TAG, "clock synchronized over BLE");
    } else {
        ESP_LOGW(TAG, "BLE clock synchronization failed: %d", errno);
    }
    return true;
}

static void receive_voice_packet(const uint8_t *packet, size_t size, bool from_ble)
{
    const uint8_t *payload = packet + HDR_BYTES;
    const size_t length = size - HDR_BYTES;
    if (packet[2] == T_AUDIO_DOWN && length > 0 && length <= OPUS_MAX_PACKET) {
        if (!s_audio_ready || !s_tts_enabled || s_talking || s_release_pending) return;
        /* Block automatic capture during playback prefill as well as audible output. */
        s_readback_queued_ms = voice_now_ms();
        if (xRingbufferSend(s_down_ring, payload, length, 0) != pdTRUE) {
            atomic_fetch_add_explicit(&s_down_dropped, 1, memory_order_relaxed);
        }
    } else if (packet[2] == T_CTRL && length > 0 && length < 64) {
        if (handle_clock_sync(payload, length, from_ble)) return;
        char cmd[64];
        memcpy(cmd, payload, length);
        cmd[length] = '\0';
        // Keepalive also reports mode and serves as a return-path heartbeat.
        if (strcmp(cmd, "ping") == 0) {
            send_tts_state();
        } else if (!from_ble && strcmp(cmd, "backend_goodbye") == 0) {
            local_voice_wifi_offline();
            ESP_LOGI(TAG, "UDP voice peer closed");
        } else {
            handle_ctrl(cmd);
        }
    }
}

static void send_hello(const char *transport)
{
    char hello[144];
    const int length = snprintf(hello, sizeof(hello),
        "{\"device\":\"amoled-1.75c\",\"rate\":%d,\"down_rate\":%d,\"ch\":%d,"
        "\"codec\":\"opus\",\"frame_ms\":%d,\"transport\":\"%s\"}",
        UPLINK_RATE, VOICE_RATE, CHANNELS, FRAME_MS, transport);
    if (length > 0 && (size_t)length < sizeof(hello)) {
        send_frame(T_HELLO, strcmp(transport, "udp") == 0 ? CAPTURE_REPORT_FLAG : 0,
                   hello, (uint16_t)length, NULL, NULL, 0);
    }
    send_tts_state();
}

static void send_udp_beacon(int64_t now_ms)
{
    const struct sockaddr_in broadcast = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        .sin_port = htons(SERVER_PORT),
    };
    const char alive[] = "{\"device\":\"amoled-1.75c\",\"state\":\"listening\"}";
    uint8_t packet[HDR_BYTES + sizeof(alive) - 1] = {
        0xA5, 0xC3, T_STATUS, 0, 0, 0, sizeof(alive) - 1,
    };
    const uint32_t timestamp = (uint32_t)now_ms;
    memcpy(packet + 8, &timestamp, sizeof(timestamp));
    memcpy(packet + HDR_BYTES, alive, sizeof(alive) - 1);
    sendto(s_sock, packet, sizeof(packet), 0, (const struct sockaddr *)&broadcast, sizeof(broadcast));
}

/* BLE has priority once its backend sends a packet. Only the active peer may
 * change the capture state or queue audio for the shared speaker. */
static void net_task(void *arg)
{
    if (!wait_for_audio_startup(arg)) {
        vTaskDelete(NULL);
        return;
    }
    static uint8_t packet[BLE_VOICE_MAX_FRAME_BYTES];
    int64_t last_ble_rx_ms = 0;
    int64_t last_udp_rx_ms = 0;
    uint32_t last_vad_poll_ms = 0;
    unsigned udp_receive_ms = UDP_IDLE_RECEIVE_MS;

    while (1) {
        const bool ble_connected = ble_voice_is_connected();
        size_t ble_size = 0;
        const bool got_ble_packet = ble_connected &&
            ble_voice_receive(packet, sizeof(packet), &ble_size, 0) == ESP_OK &&
            valid_voice_packet(packet, ble_size);
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (got_ble_packet) {
            last_ble_rx_ms = now_ms;
            if (!atomic_load(&s_ble_link_up)) {
                if (atomic_load(&s_link_up)) send_ctrl("link_lost");
                atomic_store(&s_link_up, false);
                reset_voice_audio();
                atomic_store(&s_ble_link_up, true);
                ESP_LOGI(TAG, "BLE voice peer connected");
                send_hello("ble");
            }
            receive_voice_packet(packet, ble_size, true);
        } else if (atomic_load(&s_ble_link_up) &&
                   (!ble_connected || now_ms - last_ble_rx_ms > 10000)) {
            if (ble_connected) send_ctrl("link_lost");
            atomic_store(&s_ble_link_up, false);
            reset_voice_audio();
            ESP_LOGI(TAG, "BLE voice peer disconnected");
        }
        retry_vad_capture_start();
        retry_capture_release(now_ms);
        const uint32_t ended_ms = s_vad_ended_ms;
        if (s_vad_available && s_voice_page_open && !s_talking && !s_release_pending && voice_link_up() &&
            (uint32_t)now_ms - last_vad_poll_ms >= VAD_STATUS_POLL_MS &&
            (ended_ms == 0 || (uint32_t)now_ms - ended_ms >= VAD_STATUS_POLL_MS)) {
            last_vad_poll_ms = (uint32_t)now_ms;
            send_ctrl("vad_status");
        }

        if (atomic_load(&s_ble_link_up)) {
            if (s_sock >= 0) {
                recvfrom(s_sock, packet, sizeof(packet), MSG_DONTWAIT, NULL, NULL);
            }
            if (!got_ble_packet) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }

        if (s_sock < 0) {
            s_sock = udp_open();
            if (s_sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(UDP_IDLE_RECEIVE_MS));
                continue;
            }
            udp_receive_ms = UDP_IDLE_RECEIVE_MS;
        }

        // Check control deadlines promptly while the voice page is armed,
        // including the period before a microphone onset enters grant wait.
        const bool active = atomic_load(&s_link_up) && (s_voice_page_open || s_talking || s_release_pending);
        const unsigned receive_ms = active ? UDP_ACTIVE_RECEIVE_MS : UDP_IDLE_RECEIVE_MS;
        if (receive_ms != udp_receive_ms && udp_set_receive_timeout(s_sock, receive_ms)) {
            udp_receive_ms = receive_ms;
        }
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s_sock, (char *)packet, sizeof(packet), 0,
                         (struct sockaddr *)&from, &fromlen);
        const int64_t received_ms = esp_timer_get_time() / 1000;
        if (received_ms - last_udp_rx_ms >= 10000 && atomic_exchange(&s_link_up, false)) {
            ESP_LOGW(TAG, "UDP voice peer silent for 10 s");
            reset_voice_audio();
        }

        const bool valid = n >= 0 && from.sin_family == AF_INET && valid_voice_packet(packet, (size_t)n);
        if (atomic_load(&s_link_up)) {
            if (!valid || from.sin_addr.s_addr != s_peer.sin_addr.s_addr || from.sin_port != s_peer.sin_port) {
                continue;
            }
        } else {
            static const char ping[] = "ping";
            // Only the keepalive can open a lease. Packets from a second
            // socket cannot interrupt the current capture or renew its lease.
            if (!valid || packet[2] != T_CTRL || (size_t)n != HDR_BYTES + sizeof(ping) - 1 ||
                memcmp(packet + HDR_BYTES, ping, sizeof(ping) - 1) != 0) {
                if (received_ms - s_last_beacon_ms > 1000) {
                    s_last_beacon_ms = received_ms;
                    send_udp_beacon(received_ms);
                }
                continue;
            }
            xSemaphoreTake(s_tx_lock, portMAX_DELAY);
            s_peer = from;
            atomic_store(&s_link_up, true);
            xSemaphoreGive(s_tx_lock);
            ESP_LOGI(TAG, "peer %s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            send_hello("udp");
        }
        last_udp_rx_ms = received_ms;
        receive_voice_packet(packet, (size_t)n, false);
    }
}

/* Micro-opus uses the shared pseudostack in this build. The workers keep
 * their task stacks in PSRAM and serialize codec calls with s_opus_lock. */
static bool start_opus_thread(void *(*fn)(void *), const char *name, int prio, EventGroupHandle_t startup)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8 * 1024;
    // The Opus workers do no flash writes. Keep their stacks in PSRAM so
    // audio and BLE controller DMA can share the internal memory pool.
    cfg.stack_alloc_caps = PSRAM_STACK_CAPS;
    cfg.prio = prio;
    cfg.thread_name = name;
    cfg.pin_to_core = AUDIO_CORE;
    const esp_err_t err = esp_pthread_set_cfg(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pthread_set_cfg(%s): %s", name, esp_err_to_name(err));
        return false;
    }
    pthread_t thread;
    const int rc = pthread_create(&thread, NULL, fn, startup);
    if (rc != 0) {
        ESP_LOGE(TAG, "pthread_create(%s): %d", name, rc);
        return false;
    }
    pthread_detach(thread);
    return true;
}

static esp_err_t start_audio_devices(int gain_db)
{
    ESP_RETURN_ON_ERROR(board_audio_open(&s_spk, &s_mic), TAG, "board audio initialization");
    const uint16_t mic_channels = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    const float gain = gain_db == 33 ? 34.5f : (float)gain_db;
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(s_mic, mic_channels, gain),
                        TAG, "microphone gain");
    ESP_LOGI(TAG, "ES7210 stereo microphone gain: %.1f dB", (double)gain);
    s_uplink_dsp = heap_caps_malloc(sizeof(*s_uplink_dsp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_uplink_dsp != NULL, ESP_ERR_NO_MEM, TAG, "allocate uplink DSP workspace");
    stereo_wiener_48k_init(&s_uplink_dsp->wiener);
    ESP_LOGI(TAG, "uplink Wiener: %d Hz, PCM24 stereo, %u bytes PSRAM workspace",
             UPLINK_RATE, (unsigned)sizeof(*s_uplink_dsp));
    return opus_start();
}

/* A short startup capture checks the two microphones before mixing or
 * encoding. Audio stays in the existing scratch buffers and is discarded. */
static void log_microphone_self_test(void)
{
    if (board_audio_set_capture_active(true) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone self-test could not start capture");
        return;
    }

    int64_t sum[2] = {0};
    int64_t square[2] = {0};
    int64_t cross = 0;
    unsigned peak[2] = {0};
    unsigned count = 0;
    uint32_t epoch;
    for (unsigned block = 0; block < VAD_RX_WARMUP_FRAMES + 8; ++block) {
        if (board_audio_read(s_mic_frame->pcm24, NULL, &epoch) != ESP_CODEC_DEV_OK) break;
        if (block < VAD_RX_WARMUP_FRAMES) continue;

        int16_t *samples = s_uplink_dsp->opus_pcm;
        board_audio_unpack_capture(s_mic_frame->pcm24, samples, UPLINK_SAMPLES);
        for (unsigned i = 0; i < UPLINK_SAMPLES; ++i) {
            const int left = samples[2 * i];
            const int right = samples[2 * i + 1];
            sum[0] += left;
            sum[1] += right;
            square[0] += left * left;
            square[1] += right * right;
            cross += left * right;
            if ((unsigned)abs(left) > peak[0]) peak[0] = (unsigned)abs(left);
            if ((unsigned)abs(right) > peak[1]) peak[1] = (unsigned)abs(right);
        }
        count += UPLINK_SAMPLES;
    }
    if (board_audio_set_capture_active(false) != ESP_CODEC_DEV_OK)
        ESP_LOGW(TAG, "microphone self-test could not pause capture");
    if (count == 0) {
        ESP_LOGW(TAG, "microphone self-test did not read a sample block");
        return;
    }

    const double dc_left = (double)sum[0] / count;
    const double dc_right = (double)sum[1] / count;
    const double var_left = (double)square[0] / count - dc_left * dc_left;
    const double var_right = (double)square[1] / count - dc_right * dc_right;
    const double cov = (double)cross / count - dc_left * dc_right;
    const double corr = var_left > 0 && var_right > 0 ? cov / sqrt(var_left * var_right) : 0.0;
    const double scale = (double)count * 32768.0 * 32768.0;
    const double level_left = square[0] > 0 ? 10.0 * log10((double)square[0] / scale) : -100.0;
    const double level_right = square[1] > 0 ? 10.0 * log10((double)square[1] / scale) : -100.0;
    ESP_LOGI(TAG, "microphone self-test: L %.1f dBFS peak %u dc %.1f, R %.1f dBFS peak %u dc %.1f, corr %.2f, %u frames",
             level_left, peak[0], dc_left, level_right, peak[1], dc_right, corr, count);
}

static bool start_audio_workers(void)
{
    /* The workers wait here until startup has either committed or failed. */
    static StaticEventGroup_t startup_storage;
    EventGroupHandle_t startup = xEventGroupCreateStatic(&startup_storage);
    bool ready = xTaskCreatePinnedToCore(net_task, "voice_net", 6144, startup, 6, NULL, 0) == pdPASS;
    if (!ready) ESP_LOGE(TAG, "could not start local voice network task");
    ready = ready && start_opus_thread(spk_task, "opus_spk", 7, startup) &&
            start_opus_thread(tx_task, "opus_tx", 6, startup);
    if (ready && xTaskCreatePinnedToCoreWithCaps(mic_task, "mic", 4096, startup, 8, NULL,
                                                 AUDIO_CORE, PSRAM_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "could not start microphone task");
        ready = false;
    }
    if (ready) {
        xSemaphoreTake(s_volume_lock, portMAX_DELAY);
        const int rc = esp_codec_dev_set_out_vol(s_spk, s_volume);
        if (rc != ESP_CODEC_DEV_OK) ESP_LOGE(TAG, "initial speaker volume rc=%d", rc);
        ready = rc == ESP_CODEC_DEV_OK;
        s_audio_ready = ready;
        xSemaphoreGive(s_volume_lock);
    }
    xEventGroupSetBits(startup, AUDIO_STARTUP_DONE);
    return ready;
}

static void close_audio_devices(void)
{
    if (s_enc != NULL) opus_encoder_destroy(s_enc);
    if (s_dec != NULL) opus_decoder_destroy(s_dec);
    s_enc = NULL;
    s_dec = NULL;
    heap_caps_free(s_uplink_dsp);
    s_uplink_dsp = NULL;
    board_audio_close();
    s_spk = NULL;
    s_mic = NULL;
}

static void release_audio_buffers(void)
{
    heap_caps_free(s_mic_frame);
    s_mic_frame = NULL;
    if (s_up_ring != NULL) {
        vRingbufferDeleteWithCaps(s_up_ring);
        s_up_ring = NULL;
    }
    if (s_down_ring != NULL) {
        vRingbufferDeleteWithCaps(s_down_ring);
        s_down_ring = NULL;
    }
    if (s_tx_lock != NULL) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
    }
    if (s_capture_lock != NULL) {
        vSemaphoreDelete(s_capture_lock);
        s_capture_lock = NULL;
    }
    if (s_opus_lock != NULL) {
        vSemaphoreDelete(s_opus_lock);
        s_opus_lock = NULL;
    }
}

esp_err_t local_voice_start(int volume, int mic_gain_db)
{
    if (s_audio_ready) return ESP_ERR_INVALID_STATE;
    if (volume < 0 || volume > 100 || mic_gain_db < 0 || mic_gain_db > 36)
        return ESP_ERR_INVALID_ARG;

    s_volume = volume;
    beep_init();
    // At INFO each notification can saturate the console on voice traffic.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    static StaticSemaphore_t volume_lock_storage;
    s_volume_lock = xSemaphoreCreateMutexStatic(&volume_lock_storage);
    s_tx_lock = xSemaphoreCreateMutex();
    s_capture_lock = xSemaphoreCreateMutex();
    s_opus_lock = xSemaphoreCreateMutex();

    const UBaseType_t ring_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_mic_frame = heap_caps_calloc(1, sizeof(*s_mic_frame), ring_caps);
    s_up_ring = xRingbufferCreateWithCaps(sizeof(mic_frame_t) * 16, RINGBUF_TYPE_NOSPLIT, ring_caps);
    s_down_ring = xRingbufferCreateWithCaps(OPUS_MAX_PACKET * 16, RINGBUF_TYPE_NOSPLIT, ring_caps);
    if (s_tx_lock == NULL || s_capture_lock == NULL || s_opus_lock == NULL ||
        s_mic_frame == NULL || s_up_ring == NULL || s_down_ring == NULL) {
        ESP_LOGE(TAG, "could not allocate local voice resources");
        release_audio_buffers();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = start_audio_devices(mic_gain_db);
    if (error == ESP_OK) {
        log_microphone_self_test();
        if (!start_audio_workers()) error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        close_audio_devices();
        release_audio_buffers();
        return error;
    }

    const esp_err_t ble_error = ble_voice_init();
    if (ble_error != ESP_OK)
        ESP_LOGW(TAG, "BLE voice unavailable: %s; Wi-Fi voice remains available", esp_err_to_name(ble_error));
    if (xTaskCreateWithCaps(telemetry_task, "voice_stats", 4096, NULL, 2, NULL, PSRAM_STACK_CAPS) != pdPASS)
        ESP_LOGW(TAG, "could not start local voice telemetry task");

    ESP_LOGI(TAG, "local voice started, Wi-Fi UDP :%d", SERVER_PORT);
    return ESP_OK;
}

// ESP32-S3-Touch-AMOLED-1.75C audio bridge.
//
// The board is a thin audio endpoint for a PC-side ASR/TTS app:
//   ES7210 mic  -> 16 kHz mono s16le -> TCP frames -> PC
//   PC           -> 16 kHz mono s16le -> TCP frames -> ES8311 speaker
//
// Half-duplex semantics: hold the BOOT button (GPIO0) to talk. The PC sends
// plain-text commands back on the same connection.
//
// Wire format, little endian, 12-byte header:
//   0  magic  0xA5 0xC3
//   2  type   1=HELLO 2=AUDIO_UP 3=AUDIO_DOWN 4=CTRL 5=STATUS
//   3  flags  bit0 = last frame of an utterance
//   4  seq    uint16
//   6  len    uint16 payload bytes
//   8  ts_ms  uint32 board monotonic clock
//
// This firmware is flashed into the factory app partition (0x110000); the
// factory partition table and NVS stay untouched, so the WiFi credentials
// seeded earlier are still used.
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include <pthread.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "sdkconfig.h"
#include "opus.h"

static const char *TAG = "bridge";

/* 88 px digits for the sleep clock; generated with lv_font_conv. */
LV_FONT_DECLARE(sleep_digits);

/* Short confirmation tones mixed into the speaker stream. */
#define BEEP_MAX_SAMPLES 2048
static int16_t s_beep_up[BEEP_MAX_SAMPLES];
static int16_t s_beep_down[BEEP_MAX_SAMPLES];
static int s_beep_up_len;
static int s_beep_down_len;
static const int16_t *s_beep;
static int s_beep_len;
static volatile int s_beep_pos = -1;

/* Counters sampled by telemetry_task(); one writer each, so plain increments. */
static volatile uint32_t s_up_frames;
static volatile uint32_t s_up_bytes;
static volatile uint32_t s_down_frames;
static volatile uint32_t s_down_underruns;
static volatile uint32_t s_mic_frames;
static volatile uint32_t s_mic_errors;
static volatile uint32_t s_mic_dropped;
static volatile uint32_t s_encode_errors;
static volatile uint32_t s_send_errors;
static volatile uint32_t s_decode_errors;

/* Sleep screen + panel brightness. */
static esp_lcd_panel_io_handle_t s_lcd_io;
static lv_obj_t *s_sleep_overlay;
static lv_obj_t *s_sleep_time_label;
static lv_obj_t *s_sleep_date_label;
static lv_obj_t *s_sleep_info_label;
static lv_obj_t *s_centre_time_label;
static lv_obj_t *s_status_pill;
static lv_obj_t *s_status_label;
static bool s_sleeping;
static int s_last_rssi;

static void update_status_pill(void);
static void sleep_hide(void);
static void tick_clock(void);
static volatile uint32_t s_down_idle;
static volatile int64_t s_down_last_us;

/* Battery state, filled by the PMU poll, reported by telemetry. */
static int s_battery_percent = -1;
static int s_battery_mv;
static int s_battery_current_ma;
static int s_charge_state;        /* 0 standby, 1 charging, 2 discharging */
static int s_charge_enabled;      /* 0x18 bit1 */
static int s_charge_current_set;  /* 0x62 raw, ~25 mA per step */
static bool s_battery_present;
static bool s_battery_vbus;
static bool s_battery_from_gauge;

#define SERVER_PORT     CONFIG_AUDIO_BRIDGE_SERVER_PORT

#define SAMPLE_RATE     16000
#define CHANNELS        2                   /* ES7210 MIC1 + MIC2 */
#define FRAME_SAMPLES   320                 /* per channel, 20 ms */
#define FRAME_BYTES     (FRAME_SAMPLES * 2 * CHANNELS)
#define HDR_BYTES       12


#define FRAME_MS        20
#define OPUS_MAX_PACKET 512
#define OPUS_UP_BITRATE 48000
#define BTN_GPIO        GPIO_NUM_0          /* BOOT */

enum { T_HELLO = 1, T_AUDIO_UP = 2, T_AUDIO_DOWN = 3, T_CTRL = 4, T_STATUS = 5 };

static int s_sock = -1;
static SemaphoreHandle_t s_tx_lock;
static RingbufHandle_t s_up_ring;    /* mic frames -> TCP sender */
static RingbufHandle_t s_down_ring;  /* TCP frames -> speaker */
static esp_codec_dev_handle_t s_spk, s_mic;
static OpusEncoder *s_enc;   /* 16 kHz, 2 ch: both microphones */
static OpusDecoder *s_dec;   /* 16 kHz, 1 ch: TTS downlink */
static int s_volume = 70;
static SemaphoreHandle_t s_opus_lock;   /* micro-opus runs a single pseudostack */
static volatile bool s_talking = false;
static volatile bool s_link_up = false;
static struct sockaddr_in s_peer;      /* learned from the first packet we get */
static volatile int64_t s_last_rx_ms = 0;
static char s_ip_text[24] = "-";
static char s_ssid_text[33] = "-";
static int64_t s_last_beacon_ms = 0;
static uint16_t s_seq = 0;

/* ---------------------------------------------------------------- transport */

static int send_frame(uint8_t type, uint8_t flags, const void *payload, uint16_t len)
{
    if (s_sock < 0 || !s_link_up) {
        return -1;
    }
    uint8_t hdr[HDR_BYTES];
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
    hdr[0] = 0xA5;
    hdr[1] = 0xC3;
    hdr[2] = type;
    hdr[3] = flags;
    hdr[4] = (uint8_t)(s_seq & 0xFF);
    hdr[5] = (uint8_t)(s_seq >> 8);
    hdr[6] = (uint8_t)(len & 0xFF);
    hdr[7] = (uint8_t)(len >> 8);
    hdr[8] = (uint8_t)(ts & 0xFF);
    hdr[9] = (uint8_t)((ts >> 8) & 0xFF);
    hdr[10] = (uint8_t)((ts >> 16) & 0xFF);
    hdr[11] = (uint8_t)((ts >> 24) & 0xFF);

    uint8_t packet[HDR_BYTES + 1600];
    if (len > sizeof(packet) - HDR_BYTES) {
        return -1;
    }
    memcpy(packet, hdr, HDR_BYTES);
    if (len > 0) {
        memcpy(packet + HDR_BYTES, payload, len);
    }

    int rc = -1;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        // One datagram per frame: no head-of-line blocking, a lost frame only
        // costs 20 ms instead of stalling the stream.
        int w = sendto(s_sock, (const char *)packet, HDR_BYTES + len, 0,
                       (struct sockaddr *)&s_peer, sizeof(s_peer));
        rc = (w == HDR_BYTES + len) ? 0 : -1;
        s_seq++;
        xSemaphoreGive(s_tx_lock);
    }
    return rc;
}

static void send_ctrl(const char *text)
{
    send_frame(T_CTRL, 0, text, (uint16_t)strlen(text));
    ESP_LOGI(TAG, "-> %s", text);
}

/* -------------------------------------------------------------------- audio */

static esp_err_t audio_start(void)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "bsp_audio_init");

    // bsp_audio_codec_*_init() only brings up the I2C bus when it had to do the
    // I2S init itself, so it has to be created explicitly here.
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");

    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker codec init failed (ES8311 on I2C, check the peripheral rail)");
        return ESP_FAIL;
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "mic codec init failed (ES7210 on I2C, check the peripheral rail)");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = CHANNELS,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_spk, &fs), TAG, "spk open");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &fs), TAG, "mic open");
    // The ES7210 PGA defaults to unity gain, which leaves speech around
    // -70 dBFS; 30 dB puts normal speech near -20 dBFS without clipping.
    const uint32_t mic_channels = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    esp_codec_dev_set_in_channel_gain(s_mic, mic_channels, 30.0f);
    /* Volume survives reboots; the HMI can still override it at any time. */
    nvs_handle_t handle;
    int32_t stored = 0;
    if (nvs_open("bridge", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_i32(handle, "volume", &stored) == ESP_OK) s_volume = (int)stored;
        nvs_close(handle);
    }
    if (s_volume < 0) s_volume = 0;
    if (s_volume > 100) s_volume = 100;
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_spk, s_volume), TAG, "spk vol");
    return ESP_OK;
}

/* --------------------------------------------------------------------- opus */

static esp_err_t opus_start(void)
{
    int err = OPUS_OK;
    s_enc = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || s_enc == NULL) {
        ESP_LOGE(TAG, "opus encoder create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_UP_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    s_dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK || s_dec == NULL) {
        ESP_LOGE(TAG, "opus decoder create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "opus ready: up %d bps stereo",
             OPUS_UP_BITRATE);
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

static void beep_play(const int16_t *buffer, int length)
{
    if (buffer == NULL || length <= 0) {
        return;
    }
    s_beep = buffer;
    s_beep_len = length;
    s_beep_pos = 0;
}

/* Mic -> ring. Never blocks on the network: the sender drains the ring at
 * whatever rate TCP allows and drops when the ring is full. */
static void mic_task(void *arg)
{
    (void)arg;
    static int16_t buf[FRAME_SAMPLES * CHANNELS];

    while (1) {
        if (!s_talking || !s_link_up) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int rc = esp_codec_dev_read(s_mic, buf, sizeof(buf));
        if (rc != ESP_CODEC_DEV_OK) {
            s_mic_errors++;
            ESP_LOGW(TAG, "mic read rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xRingbufferSend(s_up_ring, buf, sizeof(buf), 0) != pdTRUE) {
            s_mic_dropped++;
        }
        s_mic_frames++;
    }
}

/* Ring -> Opus -> UDP. One 20 ms frame per tick: no bursts, so the wifi/lwIP
 * transmit path is never flooded. */
static void *tx_task(void *arg)
{
    (void)arg;
    static uint8_t packet[OPUS_MAX_PACKET];

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(FRAME_MS);

    while (1) {
        vTaskDelayUntil(&last_wake, period);
        size_t size = 0;
        void *item = xRingbufferReceive(s_up_ring, &size, 0);
        if (item == NULL) {
            continue;
        }

        int n;
        xSemaphoreTake(s_opus_lock, portMAX_DELAY);
        n = opus_encode(s_enc, (const opus_int16 *)item, FRAME_SAMPLES,
                        packet, sizeof(packet));
        xSemaphoreGive(s_opus_lock);
        if (n <= 0) {
            s_encode_errors++;
        } else if (s_link_up) {
            if (send_frame(T_AUDIO_UP, 0, packet, (uint16_t)n) == 0) {
                s_up_frames++;
                s_up_bytes += (uint32_t)n;
            } else {
                s_send_errors++;
            }
        }

        vRingbufferReturnItem(s_up_ring, item);
    }
    return NULL;
}

/* Downlink ring -> speaker. The ring is the jitter buffer: when it runs dry we
 * push silence so the codec keeps its clock, and the net task drops new frames
 * when it overflows. */
static void *spk_task(void *arg)
{
    (void)arg;
    static int16_t stereo[FRAME_SAMPLES * CHANNELS];
    static int16_t mono[FRAME_SAMPLES];

        while (1) {
            size_t size = 0;
            const int64_t now_us = esp_timer_get_time();
            void *item = xRingbufferReceive(s_down_ring, &size, pdMS_TO_TICKS(20));
            int samples = 0;
            if (item != NULL) {
                if (xSemaphoreTake(s_opus_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                    samples = opus_decode(s_dec, (const uint8_t *)item, size, mono, FRAME_SAMPLES, 0);
                    xSemaphoreGive(s_opus_lock);
                }
                vRingbufferReturnItem(s_down_ring, item);
                if (samples < 0) {
                    s_decode_errors++;
                    samples = 0;
                } else {
                    s_down_frames++;
                    s_down_last_us = now_us;
                }
            }
            if (samples <= 0) {
                /* Silence while a stream is flowing is an underrun; silence with
                 * nothing to play is just the idle codec clock. */
                if (now_us - s_down_last_us < 1000000) {
                    s_down_underruns++;
                } else {
                    s_down_idle++;
                }
                memset(mono, 0, sizeof(mono));
            }
            /* The speaker codec runs on the shared stereo bus; the
             * confirmation tone is mixed on top of whatever is playing. */
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                int16_t v = (i < samples) ? mono[i] : 0;
                const int pos = s_beep_pos;
                if (pos >= 0 && pos < s_beep_len) {
                    const int mixed = v + s_beep[pos];
                    v = (int16_t)(mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
                    s_beep_pos = pos + 1;
                }
                stereo[2 * i] = v;
                stereo[2 * i + 1] = v;
            }
            if (s_beep_pos >= s_beep_len) {
                s_beep_pos = -1;
            }
            esp_codec_dev_write(s_spk, stereo, sizeof(stereo));
        }
    return NULL;
}

/* --------------------------------------------------------------- telemetry */

static void battery_update(void);

/* One line every five seconds: a support call should be answerable from a
 * serial capture alone, without reproducing the problem. */

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t up_frames = 0, up_bytes = 0, down_frames = 0, down_under = 0, down_idle = 0;
    uint32_t mic_frames = 0, mic_errors = 0, mic_dropped = 0;
    uint32_t enc_err = 0, send_err = 0, dec_err = 0;
    int64_t window_start = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        battery_update();

        const uint32_t cur_up = s_up_frames, cur_bytes = s_up_bytes;
        const uint32_t cur_down = s_down_frames, cur_under = s_down_underruns, cur_idle = s_down_idle;
        const uint32_t cur_mic = s_mic_frames, cur_mic_err = s_mic_errors, cur_mic_drop = s_mic_dropped;
        const uint32_t cur_enc = s_encode_errors, cur_send = s_send_errors, cur_dec = s_decode_errors;
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
            s_last_rssi = rssi;
        }

        ESP_LOGI(TAG,
                 "telemetry: mic=%.1ffps drop=%u err=%u | up=%.1ffps/%.1fkbps | "
                 "down=%.1ffps under=%u idle=%u dec_err=%u enc_err=%u send_err=%u | "
                 "heap=%uK min=%uK psram=%uK | rssi=%d ch=%d vol=%d | "
                 "bat=%d%%/%dmV iraw=%dmA vbus=%d gauge=%d pwr=state%d/en%d/set%dmA up=%llds",
                 (cur_mic - mic_frames) / secs, (unsigned)(cur_mic_drop - mic_dropped),
                 (unsigned)(cur_mic_err - mic_errors),
                 (cur_up - up_frames) / secs, (cur_bytes - up_bytes) * 8.0 / 1000.0 / secs,
                 (cur_down - down_frames) / secs, (unsigned)(cur_under - down_under),
                 (unsigned)(cur_idle - down_idle),
                 (unsigned)(cur_dec - dec_err), (unsigned)(cur_enc - enc_err), (unsigned)(cur_send - send_err),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 rssi, channel, s_volume, s_battery_percent, s_battery_mv,
                 s_battery_current_ma, s_battery_vbus ? 1 : 0, s_battery_from_gauge ? 1 : 0,
                 s_charge_state, s_charge_enabled, s_charge_current_set * 25,
                 (long long)(now / 1000000));

        up_frames = cur_up; up_bytes = cur_bytes;
        down_frames = cur_down; down_under = cur_under; down_idle = cur_idle;
        mic_frames = cur_mic; mic_errors = cur_mic_err; mic_dropped = cur_mic_drop;
        enc_err = cur_enc; send_err = cur_send; dec_err = cur_dec;
        window_start = now;
    }
}


/* ---------------------------------------------------------------- button */

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Hold >= HOLD_MS: push-to-talk, stop on release.
    // Two quick taps within DOUBLE_MS: hands-free on, next double tap off.
    const int64_t HOLD_MS = 300;
    const int64_t DOUBLE_MS = 500;

    bool released = true;
    bool stable = true;
    int same = 0;
    bool recording = false;
    int64_t press_ms = 0;
    int64_t last_tap_ms = -DOUBLE_MS * 2;

    while (1) {
        const bool now_released = gpio_get_level(BTN_GPIO) != 0;
        same = (now_released == released) ? same + 1 : 0;
        released = now_released;
        if (same == 3 && released != stable) {
            stable = released;
            const int64_t now = esp_timer_get_time() / 1000;
            if (!stable) {
                press_ms = now;
                if (!recording) {
                    recording = true;
                    s_talking = true;
                    beep_play(s_beep_up, s_beep_up_len);
                    send_ctrl("ptt_down");
                }
            } else {
                const int64_t held = now - press_ms;
                if (held >= HOLD_MS) {
                    if (recording) {
                        recording = false;
                        s_talking = false;
                        beep_play(s_beep_down, s_beep_down_len);
                        send_ctrl("ptt_up");
                    }
                } else if (now - last_tap_ms < DOUBLE_MS) {
                    // Second quick tap closes the hands-free session.
                    if (recording) {
                        recording = false;
                        s_talking = false;
                        send_ctrl("ptt_up");
                    }
                } else {
                    last_tap_ms = now;
                }
                ESP_LOGI(TAG, "button held %lld ms, recording=%d", held, recording);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* --------------------------------------------------------------- power key */
/* The big PWR key is wired to AXP2101 PWRON, not to an ESP32 GPIO. Short and
 * long press events land in IRQ status register 2 (0x4A); bit0 is a long press,
 * bit1 a short one. A short press toggles push-to-talk, a long press still
 * powers the board off in PMU hardware. */
#define AXP2101_ADDR        0x34


static i2c_master_dev_handle_t s_pmu;
static volatile bool s_pmu_ready = false;

static void pmu_init_once(void)
{
    const esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(),
        &(i2c_device_config_t){
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 100000,
        }, &s_pmu);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU not on the bus: %s", esp_err_to_name(err));
        return;
    }
    s_pmu_ready = true;
}
/* --------------------------------------------------------------- status ui */

static bool pmu_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_pmu_ready) return false;
    return i2c_master_transmit_receive(s_pmu, &reg, 1, buf, len, 100) == ESP_OK;
}

static bool pmu_write(uint8_t reg, uint8_t value)
{
    if (!s_pmu_ready) return false;
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_pmu, buf, 2, 100) == ESP_OK;
}

/* The AXP2101 gates its ADCs; without enabling the battery channel the
 * voltage registers read back as zero. */
/* AXP2101 gauge/ADC registers (see the datasheet and XPowersLib). */
#define AXP_STATUS1        0x00  // bit5 VBUS good, bit3 battery connected
#define AXP_ADC_CH_CTRL    0x30  // bit0 battery voltage measurement
#define AXP_BAT_VOLT_H     0x34  // 14-bit, 1 mV/LSB
#define AXP_BAT_VOLT_L     0x35
#define AXP_CHG_CURR_H     0x3C
#define AXP_CHG_CURR_L     0x3D
#define AXP_BAT_PERCENT    0xA4  // E-gauge state of charge

/* Linear interpolation over the discharge curve of a single Li-ion cell.
 * The gauge register is preferred; this is only the fallback. */
static int soc_from_voltage(int mv)
{
    static const struct {
        int mv;
        int soc;
    } curve[] = {
        {4200, 100}, {4150, 95}, {4100, 90}, {4050, 85}, {4000, 80}, {3950, 75},
        {3900, 70},  {3850, 65}, {3800, 60}, {3750, 55}, {3700, 45}, {3650, 35},
        {3600, 25},  {3550, 15}, {3500, 8},  {3400, 3},  {3300, 0},
    };
    if (mv >= curve[0].mv) return 100;
    const int last = (int)(sizeof(curve) / sizeof(curve[0])) - 1;
    if (mv <= curve[last].mv) return 0;
    for (int i = 0; i < last; i++) {
        if (mv <= curve[i].mv && mv >= curve[i + 1].mv) {
            const int span_mv = curve[i].mv - curve[i + 1].mv;
            const int span_soc = curve[i].soc - curve[i + 1].soc;
            return curve[i + 1].soc + (mv - curve[i + 1].mv) * span_soc / span_mv;
        }
    }
    return 0;
}

/* Battery state from the PMU: E-gauge percentage when it is valid, voltage
 * and pack current always, so telemetry can show charge/discharge behaviour. */
static void battery_update(void)
{
    s_battery_present = false;
    s_battery_vbus = false;

    uint8_t status = 0, adc = 0;
    if (pmu_read(AXP_STATUS1, &status, 1)) {
        s_battery_present = (status & 0x08) != 0;
        s_battery_vbus = (status & 0x20) != 0;
    }
    if (pmu_read(AXP_ADC_CH_CTRL, &adc, 1) && !(adc & 0x01)) {
        pmu_write(AXP_ADC_CH_CTRL, (uint8_t)(adc | 0x01));
    }

    uint8_t raw[2] = {0};
    s_battery_mv = 0;
    if (pmu_read(AXP_BAT_VOLT_H, raw, 2)) {
        s_battery_mv = ((raw[0] & 0x3F) << 8) | raw[1];
    }

    s_battery_current_ma = 0;
    if (pmu_read(AXP_CHG_CURR_H, raw, 2)) {
        s_battery_current_ma = (((raw[0] & 0x1F) << 8) | raw[1]) * 2;
    }

    /* Charger state: AXP2101 STATUS2[7:5] and the cell-charge enable bit. */
    uint8_t status2 = 0, gauge_ctrl = 0, icc = 0;
    s_charge_state = -1;
    if (pmu_read(0x01, &status2, 1)) {
        s_charge_state = (status2 >> 5) & 0x07;
    }
    if (pmu_read(0x18, &gauge_ctrl, 1)) {
        s_charge_enabled = (gauge_ctrl >> 1) & 0x01;
    }
    if (pmu_read(0x62, &icc, 1)) {
        s_charge_current_set = icc & 0x1F;
    }

    s_battery_percent = -1;
    uint8_t soc = 0;
    if (pmu_read(AXP_BAT_PERCENT, &soc, 1) && soc <= 100) {
        s_battery_percent = soc;   // E-gauge result, already coulomb-counted
        s_battery_from_gauge = true;
    } else if (s_battery_mv > 3000) {
        s_battery_percent = soc_from_voltage(s_battery_mv);
        s_battery_from_gauge = false;
    }

    if (!s_battery_present && s_battery_percent > 0) {
        s_battery_percent = -1;   // no pack attached: do not report a guess
    }
}


/* The stock BSP asks for a 50-line PSRAM draw buffer, so every flush needs a
 * 46 kB contiguous DMA bounce buffer; that intermittently fails once the audio
 * path is up. Register the panel with a small internal-RAM buffer instead. */
static lv_display_t *display_start(void)
{
    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&lv_cfg) != ESP_OK) {
        return NULL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&disp_config, &panel, &io) != ESP_OK) {
        return NULL;
    }
    s_lcd_io = io;
    /* bsp_display_start() used to do this; we register the panel ourselves. */
    const uint8_t brightness = 0xFF;
    esp_lcd_panel_io_tx_param(io, 0x51, &brightness, 1);
    esp_lv_adapter_display_config_t cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = 0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 50,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&cfg);
    if (disp == NULL) {
        return NULL;
    }

    /* CST9217 touch: without this the tiles cannot be tapped. */
    bsp_display_cfg_t touch_cfg = {
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    esp_lcd_touch_handle_t touch = NULL;
    if (bsp_touch_new(&touch_cfg, &touch) == ESP_OK && touch != NULL) {
        esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);
        esp_lv_adapter_register_touch(&tcfg);
    } else {
        ESP_LOGW(TAG, "touch unavailable, tiles are display-only");
    }

    esp_lv_adapter_start();
    return disp;
}

/* ---- liquid-glass dashboard ------------------------------------------- */
static lv_obj_t *s_cap_ssid;
static lv_obj_t *s_cap_vol;
static lv_obj_t *s_cap_bat;
static lv_obj_t *s_mic_tile;
static lv_obj_t *s_mic_glow;
static lv_obj_t *s_mic_ring;
static lv_anim_t s_ring_anim;
static lv_anim_t s_glow_anim;
static bool s_ring_running;
static void set_recording_motion(bool recording);
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_volume;

static void volume_apply(int value, bool persist)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    s_volume = value;
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    if (persist) {
        nvs_handle_t handle;
        if (nvs_open("bridge", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i32(handle, "volume", s_volume);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
}

/* Frosted circle: translucent tinted fill, hairline border, soft coloured
 * shadow - the closest LVGL gets to the liquid-glass look. */
static void glass_style(lv_obj_t *o, uint32_t tint, int radius)
{
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_30, 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(o, 34, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(tint), 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_y(o, 10, 0);
    lv_obj_set_scrollable(o, false);
}

/* Deliberately asynchronous: panels are also closed from a child button's
 * click handler, and deleting the ancestor mid-dispatch would be use-after-free. */
static void close_panel(void)
{
    if (s_panel != NULL) {
        lv_obj_delete_async(s_panel);
        s_panel = NULL;
        s_panel_volume = NULL;
    }
}

static void on_dismiss(lv_event_t *e)
{
    (void)e;
    close_panel();
}

static lv_obj_t *panel_open(const char *title, uint32_t tint)
{
    close_panel();
    s_panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_scrollable(s_panel, false);
    lv_obj_add_event_cb(s_panel, on_dismiss, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_panel);
    lv_obj_set_size(card, 390, 300);
    glass_style(card, tint, 30);
    lv_obj_set_clickable(card, true);
    lv_obj_set_scrollable(card, false);
    lv_obj_center(card);

    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
    return card;
}

static void on_sound(lv_event_t *e);
static void on_volume_up(lv_event_t *e);

static void volume_step(int delta)
{
    volume_apply(s_volume + delta, true);
    if (s_panel_volume != NULL) {
        char text[8];
        snprintf(text, sizeof(text), "%d%%", s_volume);
        lv_label_set_text(s_panel_volume, text);
    }
}

static void on_volume_down(lv_event_t *e)
{
    (void)e;
    volume_step(-10);
}

static void on_wlan(lv_event_t *e)
{
    (void)e;
    lv_obj_t *card = panel_open(LV_SYMBOL_WIFI "  " "WLAN", 0x2C6E9B);
    lv_obj_t *body = lv_label_create(card);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xDCE9F7), 0);
    lv_obj_set_style_text_line_space(body, 6, 0);
    char lines[160];
    snprintf(lines, sizeof(lines), "SSID  %s\nIP    %s\nPeer  %s", s_ssid_text, s_ip_text,
             s_link_up ? "connected" : "waiting");
    lv_label_set_text(body, lines);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 16, 60);
}

static void on_info(lv_event_t *e)
{
    (void)e;
    lv_obj_t *card = panel_open(LV_SYMBOL_BATTERY_3 "  " "Power", 0x2E7D5B);
    lv_obj_t *body = lv_label_create(card);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xDCE9F7), 0);
    lv_obj_set_style_text_line_space(body, 6, 0);
    char lines[160];
    snprintf(lines, sizeof(lines), "Battery  %d%%\nVoltage  %d.%02d V\nVolume   %d%%",
             s_battery_percent < 0 ? 0 : s_battery_percent,
             s_battery_mv / 1000, (s_battery_mv % 1000) / 10, s_volume);
    lv_label_set_text(body, lines);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 16, 60);
}

static void on_sound(lv_event_t *e)
{
    (void)e;
    lv_obj_t *card = panel_open(LV_SYMBOL_VOLUME_MAX "  " "Sound", 0x6B4E9B);
    lv_obj_t *body = lv_label_create(card);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", s_volume);
    lv_label_set_text(body, percent);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 70);
    s_panel_volume = body;

    lv_obj_t *minus = lv_button_create(card);
    lv_obj_set_size(minus, 96, 64);
    lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 24, -20);
    lv_obj_add_event_cb(minus, on_volume_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_center(ml);

    lv_obj_t *plus = lv_button_create(card);
    lv_obj_set_size(plus, 96, 64);
    lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -24, -20);
    lv_obj_add_event_cb(plus, on_volume_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_center(pl);
}

static void on_volume_up(lv_event_t *e)
{
    (void)e;
    volume_step(10);
}

static void set_caption(lv_obj_t *label, const char *text, char *cache, size_t cache_size)
{
    if (label == NULL || strcmp(cache, text) == 0) return;
    snprintf(cache, cache_size, "%s", text);
    lv_label_set_text(label, cache);
}

/* Text is formatted with snprintf: LVGL's builtin sprintf mangles "%%", and
 * only changed values are pushed so the panel is not redrawn every tick. */
static void refresh_captions(void)
{
    static char ssid_cache[40], vol_cache[8], bat_cache[8];
    char buffer[40];

    set_caption(s_cap_ssid, s_ssid_text, ssid_cache, sizeof(ssid_cache));

    snprintf(buffer, sizeof(buffer), "%d%%", s_volume);
    set_caption(s_cap_vol, buffer, vol_cache, sizeof(vol_cache));

    snprintf(buffer, sizeof(buffer), "%d%%", s_battery_percent < 0 ? 0 : s_battery_percent);

    set_caption(s_cap_bat, buffer, bat_cache, sizeof(bat_cache));


    /* The centre puck doubles as the state indicator: red while recording. */
    static bool tinted = false;
    if (s_mic_tile != NULL && tinted != s_talking) {
        tinted = s_talking;
        const uint32_t tint = s_talking ? 0xC0392B : 0x2F6FEB;
        lv_obj_set_style_bg_color(s_mic_tile, lv_color_hex(tint), 0);
        lv_obj_set_style_shadow_color(s_mic_tile, lv_color_hex(tint), 0);
        set_recording_motion(s_talking);
    }

    /* Capture can also start from the PC hotkey; never record behind a dark
     * screen, the user has to see the ON AIR state. */
    if (s_talking && s_sleeping) {
        sleep_hide();
    }
    update_status_pill();
}



/* Board-side capture switch: the round UI had it on the screen, so a tap on
 * the microphone puck starts and stops the uplink without touching BOOT. */
static void on_mic(lv_event_t *e)
{
    (void)e;
    if (s_talking) {
        s_talking = false;
        beep_play(s_beep_down, s_beep_down_len);
        send_ctrl("ptt_up");
    } else if (s_link_up) {
        s_talking = true;
        beep_play(s_beep_up, s_beep_up_len);
        send_ctrl("ptt_down");
    } else {
        /* No peer: the status pill already reads NO LINK, nothing else to do. */
        refresh_captions();
        return;
    }
    refresh_captions();
}

/* ------------------------------------------------------------ ui motion */


static void ring_rotate_cb(void *var, int32_t value)
{
    lv_arc_set_rotation((lv_obj_t *)var, (int16_t)value);
}

static void glow_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void puck_pressed(lv_event_t *e)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)lv_event_get_target(e), 243, 0);
}

static void puck_released(lv_event_t *e)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)lv_event_get_target(e), 256, 0);
}

/* Recording: a bright 90 degree segment sweeps the ring and the glow breathes.
 * Idle: a static 300 degree hairline, like a watch complication. */
static void set_recording_motion(bool recording)
{
    if (s_mic_ring == NULL) {
        return;
    }
    if (recording == s_ring_running) {
        return;
    }
    s_ring_running = recording;

    if (recording) {
        lv_arc_set_angles(s_mic_ring, 0, 90);
        lv_obj_set_style_arc_opa(s_mic_ring, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_mic_ring, lv_color_hex(0xFF9B8A), LV_PART_INDICATOR);

        lv_anim_init(&s_ring_anim);
        lv_anim_set_var(&s_ring_anim, s_mic_ring);
        lv_anim_set_exec_cb(&s_ring_anim, ring_rotate_cb);
        lv_anim_set_values(&s_ring_anim, 0, 360);
        lv_anim_set_duration(&s_ring_anim, 1600);
        lv_anim_set_repeat_count(&s_ring_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&s_ring_anim);

        lv_anim_init(&s_glow_anim);
        lv_anim_set_var(&s_glow_anim, s_mic_glow);
        lv_anim_set_exec_cb(&s_glow_anim, glow_opa_cb);
        lv_anim_set_values(&s_glow_anim, LV_OPA_40, LV_OPA_10);
        lv_anim_set_duration(&s_glow_anim, 900);
        lv_anim_set_playback_duration(&s_glow_anim, 900);
        lv_anim_set_repeat_count(&s_glow_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&s_glow_anim);
    } else {
        lv_anim_delete(s_mic_ring, ring_rotate_cb);
        lv_anim_delete(s_mic_glow, glow_opa_cb);
        lv_arc_set_rotation(s_mic_ring, 0);
        lv_arc_set_angles(s_mic_ring, 0, 300);
        lv_obj_set_style_arc_opa(s_mic_ring, LV_OPA_30, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_mic_ring, lv_color_hex(0x9CC4FF), LV_PART_INDICATOR);
        lv_obj_set_style_opa(s_mic_glow, LV_OPA_20, 0);
    }
}

/* ------------------------------------------------------------------ clock */

/* The panel only ships Latin fonts, so the date stays ASCII. */
static bool clock_now(struct tm *local)
{
    const time_t now = time(NULL);
    if (now < 1700000000) { /* SNTP has not answered yet */
        return false;
    }
    localtime_r(&now, local);
    return true;
}

static void clock_time_text(char *out, size_t size)
{
    struct tm local;
    if (!clock_now(&local)) {
        snprintf(out, size, "--:--");
        return;
    }
    strftime(out, size, "%H:%M", &local);
}

static void clock_date_text(char *out, size_t size)
{
    static const char *names[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    struct tm local;
    if (!clock_now(&local)) {
        snprintf(out, size, "--");
        return;
    }
    snprintf(out, size, "%s  %d", names[local.tm_wday % 7], local.tm_mday);
}

/* ------------------------------------------------------------------ sleep */

static void set_backlight(uint8_t value)
{
    if (s_lcd_io != NULL) {
        esp_lcd_panel_io_tx_param(s_lcd_io, 0x51, &value, 1);
    }
}

static void overlay_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void overlay_faded_out(lv_anim_t *anim)
{
    (void)anim;
    if (!s_sleeping && s_sleep_overlay != NULL) {
        lv_obj_set_hidden(s_sleep_overlay, true);
        lv_obj_set_style_opa(s_sleep_overlay, LV_OPA_COVER, 0);
    }
}

static void sleep_show(void)
{
    if (s_sleeping || s_sleep_overlay == NULL) {
        return;
    }
    s_sleeping = true;
    lv_obj_set_hidden(s_sleep_overlay, false);
    lv_obj_move_foreground(s_sleep_overlay);
    lv_anim_delete(s_sleep_overlay, overlay_opa_cb);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_sleep_overlay);
    lv_anim_set_exec_cb(&fade, overlay_opa_cb);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 220);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_start(&fade);

    set_backlight(0x28);
}

static void sleep_hide(void)
{
    if (!s_sleeping || s_sleep_overlay == NULL) {
        return;
    }
    s_sleeping = false;
    lv_anim_delete(s_sleep_overlay, overlay_opa_cb);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_sleep_overlay);
    lv_anim_set_exec_cb(&fade, overlay_opa_cb);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 180);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&fade, overlay_faded_out);
    lv_anim_start(&fade);

    set_backlight(0xFF);
}

static void sleep_touched(lv_event_t *e)
{
    (void)e;
    sleep_hide();
}

/* ------------------------------------------------------------------ status */

static void update_status_pill(void)
{
    if (s_status_label == NULL || s_status_pill == NULL) {
        return;
    }
    if (s_talking) {
        lv_label_set_text(s_status_label, "ON AIR");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFC4BC), 0);
        lv_obj_set_style_bg_opa(s_status_pill, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(0xFF9484), 0);
        lv_obj_set_style_border_color(s_status_pill, lv_color_hex(0xFFAA9E), 0);
    } else {
        lv_label_set_text(s_status_label, s_link_up ? "READY" : "NO LINK");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC9D8EC), 0);
        lv_obj_set_style_bg_opa(s_status_pill, LV_OPA_10, 0);
        lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_color(s_status_pill, lv_color_hex(0xFFFFFF), 0);
    }
}

static void tick_clock(void)
{
    char buffer[48];
    clock_time_text(buffer, sizeof(buffer));
    if (s_centre_time_label != NULL) {
        lv_label_set_text(s_centre_time_label, buffer);
    }
    if (s_sleep_time_label != NULL) {
        lv_label_set_text(s_sleep_time_label, buffer);
    }
    if (s_sleep_date_label != NULL) {
        clock_date_text(buffer, sizeof(buffer));
        lv_label_set_text(s_sleep_date_label, buffer);
    }
    if (s_sleep_info_label != NULL) {
        snprintf(buffer, sizeof(buffer), "WiFi %d dBm    BAT %d%%",
                 s_last_rssi, s_battery_percent < 0 ? 0 : s_battery_percent);
        lv_label_set_text(s_sleep_info_label, buffer);
    }
}

/* --------------------------------------------------------------------- ui */

static void ui_start(void)
{
    if (display_start() == NULL) {
        ESP_LOGW(TAG, "display unavailable, running headless");
        return;
    }
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070D), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0E1B33), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Ambient light: three faint discs give the glass something to sit on. */
    static const struct { int x, y, r; uint32_t color; lv_opa_t opa; } blooms[] = {
        { -140, -170, 200, 0x2F6FEB, LV_OPA_10 },
        { 210, 120, 210, 0x7046A8, LV_OPA_10 },
        { 150, -180, 140, 0x2C7AA8, LV_OPA_10 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bloom = lv_obj_create(screen);
        lv_obj_set_size(bloom, blooms[i].r, blooms[i].r);
        lv_obj_set_style_radius(bloom, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bloom, lv_color_hex(blooms[i].color), 0);
        lv_obj_set_style_bg_opa(bloom, blooms[i].opa, 0);
        lv_obj_set_style_border_width(bloom, 0, 0);
        lv_obj_set_style_shadow_width(bloom, 60, 0);
        lv_obj_set_style_shadow_color(bloom, lv_color_hex(blooms[i].color), 0);
        lv_obj_set_style_shadow_opa(bloom, LV_OPA_10, 0);
        lv_obj_set_scrollable(bloom, false);
        lv_obj_set_clickable(bloom, false);
        lv_obj_align(bloom, LV_ALIGN_CENTER, blooms[i].x / 2, blooms[i].y / 2);
    }

    /* Honeycomb: six chips on a ring, the central puck is the capture switch. */
    struct Satellite {
        const char *symbol;
        uint32_t tint;
        lv_event_cb_t cb;
        lv_obj_t **caption;
        int x, y;
    };
    static const struct Satellite satellites[6] = {
        { LV_SYMBOL_WIFI, 0x2C7AA8, on_wlan, &s_cap_ssid, -112, -112 },
        { LV_SYMBOL_VOLUME_MAX, 0x7046A8, on_sound, &s_cap_vol, 112, -112 },
        { LV_SYMBOL_BATTERY_3, 0x2E8C64, on_info, &s_cap_bat, 160, 0 },
        { LV_SYMBOL_SETTINGS, 0x4E5A6C, on_info, NULL, 112, 112 },
        { LV_SYMBOL_LIST, 0x4E5A6C, on_info, NULL, -112, 112 },
        { LV_SYMBOL_AUDIO, 0x2F6FEB, on_mic, NULL, -160, 0 },
    };

    for (int i = 0; i < 6; i++) {
        lv_obj_t *chip = lv_obj_create(screen);
        lv_obj_set_size(chip, 76, 76);
        glass_style(chip, satellites[i].tint, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(chip, true);
        lv_obj_add_event_cb(chip, satellites[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(chip, puck_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(chip, puck_released, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(chip, puck_released, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_set_scrollable(chip, false);
        lv_obj_align(chip, LV_ALIGN_CENTER, satellites[i].x, satellites[i].y);

        lv_obj_t *icon = lv_label_create(chip);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        lv_label_set_text(icon, satellites[i].symbol);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -8);

        if (satellites[i].caption != NULL) {
            lv_obj_t *label = lv_label_create(chip);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xC9D8EC), 0);
            lv_obj_set_width(label, 70);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_label_set_text(label, "--");
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 20);
            *satellites[i].caption = label;
        }
    }

    /* Glow + recording ring + glass puck. */
    s_mic_glow = lv_obj_create(screen);
    lv_obj_set_size(s_mic_glow, 208, 208);
    lv_obj_set_style_radius(s_mic_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_glow, lv_color_hex(0x2F6FEB), 0);
    lv_obj_set_style_bg_opa(s_mic_glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_mic_glow, 0, 0);
    lv_obj_set_style_shadow_width(s_mic_glow, 60, 0);
    lv_obj_set_style_shadow_color(s_mic_glow, lv_color_hex(0x2F6FEB), 0);
    lv_obj_set_style_shadow_opa(s_mic_glow, LV_OPA_30, 0);
    lv_obj_set_scrollable(s_mic_glow, false);
    lv_obj_set_clickable(s_mic_glow, false);
    lv_obj_align(s_mic_glow, LV_ALIGN_CENTER, 0, 0);

    s_mic_ring = lv_arc_create(screen);
    lv_obj_set_size(s_mic_ring, 190, 190);
    lv_obj_align(s_mic_ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(s_mic_ring, 0, 100);
    lv_arc_set_value(s_mic_ring, 100);
    lv_arc_set_bg_angles(s_mic_ring, 0, 360);
    lv_arc_set_angles(s_mic_ring, 0, 300);
    lv_obj_set_style_bg_opa(s_mic_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mic_ring, 0, 0);
    lv_obj_set_style_arc_width(s_mic_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_mic_ring, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_mic_ring, lv_color_hex(0x9CC4FF), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mic_ring, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_mic_ring, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_mic_ring, lv_color_hex(0x9CC4FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_mic_ring, true, LV_PART_INDICATOR);
    lv_obj_remove_style(s_mic_ring, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_mic_ring, false);

    s_mic_tile = lv_obj_create(screen);
    lv_obj_set_size(s_mic_tile, 176, 176);
    glass_style(s_mic_tile, 0x2F6FEB, LV_RADIUS_CIRCLE);
    lv_obj_set_clickable(s_mic_tile, true);
    lv_obj_add_event_cb(s_mic_tile, on_mic, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_mic_tile, puck_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_mic_tile, puck_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_mic_tile, puck_released, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_scrollable(s_mic_tile, false);
    lv_obj_align(s_mic_tile, LV_ALIGN_CENTER, 0, 0);

    s_centre_time_label = lv_label_create(s_mic_tile);
    lv_obj_set_style_text_font(s_centre_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_centre_time_label, lv_color_hex(0xF5FAFF), 0);
    lv_label_set_text(s_centre_time_label, "--:--");
    lv_obj_align(s_centre_time_label, LV_ALIGN_CENTER, 0, -26);

    s_status_pill = lv_obj_create(s_mic_tile);
    lv_obj_set_size(s_status_pill, 108, 28);
    lv_obj_set_style_radius(s_status_pill, 14, 0);
    lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_status_pill, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_status_pill, 1, 0);
    lv_obj_set_style_border_color(s_status_pill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(s_status_pill, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(s_status_pill, 0, 0);
    lv_obj_set_scrollable(s_status_pill, false);
    lv_obj_set_clickable(s_status_pill, false);
    lv_obj_align(s_status_pill, LV_ALIGN_CENTER, 0, 30);

    s_status_label = lv_label_create(s_status_pill);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC9D8EC), 0);
    lv_label_set_text(s_status_label, "READY");
    lv_obj_center(s_status_label);

    lv_obj_t *hint = lv_label_create(s_mic_tile);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB9CCE4), 0);
    lv_label_set_text(hint, "TAP TO TALK");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 60);

    /* Sleep overlay: opaque, covers everything, shown by the idle timer. */
    s_sleep_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_sleep_overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_radius(s_sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_sleep_overlay, lv_color_hex(0x03050A), 0);
    lv_obj_set_style_bg_opa(s_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sleep_overlay, 0, 0);
    lv_obj_set_scrollable(s_sleep_overlay, false);
    lv_obj_set_clickable(s_sleep_overlay, true);
    lv_obj_add_event_cb(s_sleep_overlay, sleep_touched, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_sleep_overlay, sleep_touched, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_center(s_sleep_overlay);

    s_sleep_time_label = lv_label_create(s_sleep_overlay);
    lv_obj_set_style_text_font(s_sleep_time_label, &sleep_digits, 0);
    lv_obj_set_style_text_color(s_sleep_time_label, lv_color_hex(0xE8F0FC), 0);
    lv_label_set_text(s_sleep_time_label, "--:--");
    lv_obj_align(s_sleep_time_label, LV_ALIGN_CENTER, 0, -60);

    s_sleep_date_label = lv_label_create(s_sleep_overlay);
    lv_obj_set_style_text_font(s_sleep_date_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_sleep_date_label, lv_color_hex(0x9FB3CC), 0);
    lv_label_set_text(s_sleep_date_label, "");
    lv_obj_align(s_sleep_date_label, LV_ALIGN_CENTER, 0, 4);

    s_sleep_info_label = lv_label_create(s_sleep_overlay);
    lv_obj_set_style_text_font(s_sleep_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sleep_info_label, lv_color_hex(0x8FA8C4), 0);
    lv_label_set_text(s_sleep_info_label, "");
    lv_obj_align(s_sleep_info_label, LV_ALIGN_CENTER, 0, 62);

    lv_obj_t *wake = lv_label_create(s_sleep_overlay);
    lv_obj_set_style_text_font(wake, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wake, lv_color_hex(0x6E819B), 0);
    lv_label_set_text(wake, "TAP TO WAKE");
    lv_obj_align(wake, LV_ALIGN_CENTER, 0, 122);

    lv_obj_set_hidden(s_sleep_overlay, true);

    refresh_captions();
    update_status_pill();
    tick_clock();
    bsp_display_unlock();
}



static void ui_task(void *arg)
{
    (void)arg;
    ui_start();
    int ticks = 0;
    while (1) {
        if (++ticks % 20 == 0) {
            battery_update();
        }
        if (s_cap_ssid != NULL && bsp_display_lock(200) == ESP_OK) {
            refresh_captions();
            if (ticks % 2 == 0) {
                tick_clock();
                if (!s_sleeping && !s_talking &&
                    lv_display_get_inactive_time(NULL) > 30000) {
                    sleep_show();
                }
            }
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
/* ------------------------------------------------------------------ network */

static esp_timer_handle_t s_wifi_retry_timer;
static int s_wifi_retries;
static uint32_t s_wifi_backoff_ms = 1000;

static void wifi_retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* Reconnects with capped exponential backoff and, more importantly, logs why:
 * without the reason code a silent drop looks identical to a dead board. */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *c = (const wifi_event_sta_connected_t *)data;
        s_wifi_retries = 0;
        s_wifi_backoff_ms = 1000;
        ESP_LOGI(TAG, "wifi associated: ssid=\"%.32s\" channel=%d",
                 (const char *)c->ssid, c->channel);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_link_up = false;
        s_wifi_retries++;
        const uint32_t backoff = s_wifi_backoff_ms;
        ESP_LOGW(TAG, "wifi disconnected: reason=%d retry=%d backoff=%ums ssid=\"%.32s\"",
                 d != NULL ? d->reason : -1, s_wifi_retries, (unsigned)backoff, s_ssid_text);
        s_wifi_backoff_ms = backoff >= 30000 ? 30000 : backoff * 2;
        if (s_wifi_retry_timer != NULL) {
            esp_timer_stop(s_wifi_retry_timer);
            esp_timer_start_once(s_wifi_retry_timer, (uint64_t)backoff * 1000);
        } else {
            esp_wifi_connect();
        }
    }
}


static EventGroupHandle_t s_ip_events;
#define IP_BIT BIT0

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_ip_text, sizeof(s_ip_text), IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ip_events, IP_BIT);

        /* The panel has no backed RTC: sync once so the clock face is real. */
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "sntp started");
        }
    }
}

static esp_err_t wifi_start(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    if (s_wifi_retry_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = wifi_retry_cb,
            .name = "wifi_retry",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_wifi_retry_timer), TAG, "wifi retry timer");
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL), TAG, "ip evt");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    wifi_config_t cfg = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &cfg), TAG, "get config");
    if (cfg.sta.ssid[0] == 0) {
        ESP_LOGE(TAG, "no SSID in NVS - seed credentials first (see wifi_seed)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "using stored SSID \"%s\"", (char *)cfg.sta.ssid);
    snprintf(s_ssid_text, sizeof(s_ssid_text), "%.32s", (char *)cfg.sta.ssid);

    // Modem sleep adds 100-300 ms of latency on every burst, which shows up as
    // 12-18 fps instead of 50 on a 20 ms audio stream.
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_ip_events = xEventGroupCreate();
    if (!(xEventGroupWaitBits(s_ip_events, IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000)) & IP_BIT)) {
        ESP_LOGW(TAG, "no IP yet, will keep retrying in background");
    }
    return ESP_OK;
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
    // Wake the receive loop regularly so the peer watchdog and the discovery
    // beacon can run.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "udp listening on :%d", SERVER_PORT);
    return sock;
}

static void handle_ctrl(const char *cmd)
{
    ESP_LOGI(TAG, "<- %s", cmd);
    if (strcmp(cmd, "stop_playback") == 0) {
        size_t size = 0;
        void *item;
        while ((item = xRingbufferReceive(s_down_ring, &size, 0)) != NULL) {
            vRingbufferReturnItem(s_down_ring, item);
        }
    } else if (strcmp(cmd, "capture_start") == 0) {
        s_talking = true;
    } else if (strcmp(cmd, "capture_stop") == 0) {
        s_talking = false;
    } else if (strncmp(cmd, "volume:", 7) == 0) {
        volume_apply(atoi(cmd + 7), true);
    }
}


/* Datagram receive loop. The peer address is learned from the first valid
 * packet, so the PC only has to know the board's IP; a peer that goes quiet for
 * 10 s is forgotten and the next HELLO re-establishes the session. */
static void net_task(void *arg)
{
    (void)arg;
    static uint8_t packet[HDR_BYTES + 1600];
    static uint8_t payload[1600];

    s_sock = -1;
    while (1) {
        if (s_sock < 0) {
            s_sock = udp_open();
            if (s_sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s_sock, (char *)packet, sizeof(packet), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < HDR_BYTES) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (s_link_up && (now_ms - s_last_rx_ms) > 10000) {
                    ESP_LOGW(TAG, "peer silent for 10 s, forgetting it");
                    s_link_up = false;
                }
                // Until someone talks to us, announce ourselves so the PC can
                // discover the board without knowing its DHCP lease.
                if (!s_link_up && (now_ms - s_last_beacon_ms) > 1000) {
                    s_last_beacon_ms = now_ms;
                    struct sockaddr_in bcast = {0};
                    bcast.sin_family = AF_INET;
                    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                    bcast.sin_port = htons(SERVER_PORT);
                    const char *alive = "{\"device\":\"amoled-1.75c\",\"state\":\"listening\"}";
                    uint8_t pkt[HDR_BYTES + 64];
                    pkt[0] = 0xA5; pkt[1] = 0xC3; pkt[2] = T_STATUS; pkt[3] = 0;
                    pkt[4] = 0; pkt[5] = 0;
                    pkt[6] = (uint8_t)strlen(alive); pkt[7] = 0;
                    uint32_t ts = (uint32_t)now_ms;
                    memcpy(pkt + 8, &ts, 4);
                    memcpy(pkt + HDR_BYTES, alive, strlen(alive));
                    sendto(s_sock, (const char *)pkt, HDR_BYTES + strlen(alive), 0,
                           (struct sockaddr *)&bcast, sizeof(bcast));
                }
            }
            continue;
        }
        if (packet[0] != 0xA5 || packet[1] != 0xC3) {
            continue;
        }
        uint8_t type = packet[2];
        uint16_t len = (uint16_t)(packet[6] | (packet[7] << 8));
        if (len != n - HDR_BYTES || len > sizeof(payload)) {
            continue;
        }
        memcpy(payload, packet + HDR_BYTES, len);

        s_last_rx_ms = esp_timer_get_time() / 1000;
        bool was_up = s_link_up;
        if (!was_up || memcmp(&from, &s_peer, sizeof(from)) != 0) {
            s_peer = from;
            s_link_up = true;
            ESP_LOGI(TAG, "peer %s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            char hello[96];
            snprintf(hello, sizeof(hello),
                     "{\"device\":\"amoled-1.75c\",\"rate\":%d,\"ch\":%d,\"codec\":\"opus\",\"frame_ms\":%d,\"transport\":\"udp\"}",
                     SAMPLE_RATE, CHANNELS, FRAME_MS);
            send_frame(T_HELLO, 0, hello, (uint16_t)strlen(hello));
        }

        if (type == T_AUDIO_DOWN) {
            if (xRingbufferSend(s_down_ring, payload, len, 0) != pdTRUE) {
                ESP_LOGD(TAG, "playback ring full, dropped %u bytes", len);
            }
        } else if (type == T_CTRL) {
            char cmd[64] = {0};
            snprintf(cmd, sizeof(cmd), "%.*s", len, (char *)payload);
            handle_ctrl(cmd);
        }
    }
}

/* micro-opus gives every thread its own Opus pseudostack through pthread TLS,
 * so both codec users must run as pthreads rather than plain FreeRTOS tasks. */
static void start_opus_thread(pthread_t *th, void *(*fn)(void *), const char *name, int prio)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8 * 1024;
    cfg.prio = prio;
    cfg.thread_name = name;
    cfg.pin_to_core = 0;
    esp_pthread_set_cfg(&cfg);
    if (pthread_create(th, NULL, fn, NULL) != 0) {
        ESP_LOGE(TAG, "pthread_create(%s) failed", name);
    }
}

/* --------------------------------------------------------------------- main */

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    beep_init();
    /* The adapter logs a few lock timeouts while the first frame renders;
     * they are harmless and drown out real errors in a serial capture. */
    esp_log_level_set("esp_lvgl:adapter", ESP_LOG_NONE);
    s_tx_lock = xSemaphoreCreateMutex();
    s_opus_lock = xSemaphoreCreateMutex();
    /* NOSPLIT rings keep one frame per item, so the wire format can never be
     * cut in the middle of a frame. 16 frames = 320 ms of slack. */
    s_up_ring = xRingbufferCreate(FRAME_BYTES * 16, RINGBUF_TYPE_NOSPLIT);
    s_down_ring = xRingbufferCreate(OPUS_MAX_PACKET * 16, RINGBUF_TYPE_NOSPLIT);

    if (wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi unavailable, bridge idle");
    }
    // Display first: LVGL and the panel need DMA-capable internal RAM, which
    // the I2S/Opus allocations would otherwise fragment.
    xTaskCreate(ui_task, "ui", 12288, NULL, 3, NULL);

    if (audio_start() != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable, bridge idle");
    }
    if (opus_start() != ESP_OK) {
        ESP_LOGE(TAG, "opus unavailable, bridge idle");
    }

    pthread_t spk_th, tx_th;
    start_opus_thread(&spk_th, spk_task, "opus_spk", 6);
    start_opus_thread(&tx_th, tx_task, "opus_tx", 5);
    xTaskCreate(mic_task, "mic", 4096, NULL, 7, NULL);
    xTaskCreate(button_task, "btn", 3072, NULL, 4, NULL);
    pmu_init_once();
    xTaskCreate(net_task, "net", 6144, NULL, 5, NULL);

    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 2, NULL);


    ESP_LOGI(TAG, "bridge up: udp :%d, hold BOOT to talk", SERVER_PORT);
}
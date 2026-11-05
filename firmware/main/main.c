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

#define SERVER_IP       CONFIG_AUDIO_BRIDGE_SERVER_IP
#define SERVER_PORT     CONFIG_AUDIO_BRIDGE_SERVER_PORT

#define SAMPLE_RATE     16000
#define CHANNELS        2                   /* ES7210 MIC1 + MIC2 */
#define FRAME_SAMPLES   320                 /* per channel, 20 ms */
#define FRAME_BYTES     (FRAME_SAMPLES * 2 * CHANNELS)
#define HDR_BYTES       12


#define FRAME_MS        20
#define OPUS_MAX_PACKET 512
#define OPUS_UP_BITRATE 48000
#define OPUS_DOWN_BITRATE 24000
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
    ESP_LOGI(TAG, "opus ready: up %d bps stereo, down %d bps mono",
             OPUS_UP_BITRATE, OPUS_DOWN_BITRATE);
    return ESP_OK;
}

/* Mic -> ring. Never blocks on the network: the sender drains the ring at
 * whatever rate TCP allows and drops when the ring is full. */
static void mic_task(void *arg)
{
    (void)arg;
    static int16_t buf[FRAME_SAMPLES * CHANNELS];
    uint32_t frames = 0;
    uint32_t errors = 0;
    uint32_t dropped = 0;
    int64_t window_start = esp_timer_get_time();

    while (1) {
        if (!s_talking || !s_link_up) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int rc = esp_codec_dev_read(s_mic, buf, sizeof(buf));
        if (rc != ESP_CODEC_DEV_OK) {
            errors++;
            ESP_LOGW(TAG, "mic read rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xRingbufferSend(s_up_ring, buf, sizeof(buf), 0) != pdTRUE) {
            dropped++;
        }
        frames++;

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 2000000) {
            ESP_LOGI(TAG, "capture %.1f fps (want 50), %u read errors, %u dropped",
                     frames * 1e6 / (double)(now - window_start),
                     (unsigned)errors, (unsigned)dropped);
            frames = 0;
            errors = 0;
            dropped = 0;
            window_start = now;
        }
    }
}

/* Ring -> Opus -> UDP. One 20 ms frame per tick: no bursts, so the wifi/lwIP
 * transmit path is never flooded. */
static void *tx_task(void *arg)
{
    (void)arg;
    static uint8_t packet[OPUS_MAX_PACKET];

    uint32_t sent = 0;
    uint32_t window_frames = 0;
    uint32_t window_bytes = 0;
    int64_t window_start = esp_timer_get_time();

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
            ESP_LOGW(TAG, "opus encode failed: %d", n);
        } else if (s_link_up) {
            if (send_frame(T_AUDIO_UP, 0, packet, (uint16_t)n) == 0) {
                window_frames++;
                window_bytes += (uint32_t)n;
            } else {
                ESP_LOGW(TAG, "uplink send failed errno=%d", errno);
            }
        }

        vRingbufferReturnItem(s_up_ring, item);
        sent++;
        int64_t now = esp_timer_get_time();
        if (now - window_start >= 2000000) {
            double secs = (now - window_start) / 1e6;
            ESP_LOGI(TAG, "tx %.1f fps, %u B/s (%u kbps), total %u",
                     window_frames / secs,
                     (unsigned)(window_bytes / secs),
                     (unsigned)(window_bytes * 8 / secs / 1000),
                     (unsigned)sent);
            window_frames = 0;
            window_bytes = 0;
            window_start = now;
        }
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
    uint32_t underruns = 0;
    uint32_t window_frames = 0;
    int64_t window_start = esp_timer_get_time();

    while (1) {
        size_t size = 0;
        void *item = xRingbufferReceive(s_down_ring, &size, pdMS_TO_TICKS(20));
        int samples = 0;
        if (item != NULL) {
            if (xSemaphoreTake(s_opus_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                samples = opus_decode(s_dec, (const uint8_t *)item, size, mono, FRAME_SAMPLES, 0);
                xSemaphoreGive(s_opus_lock);
            }
            vRingbufferReturnItem(s_down_ring, item);
            if (samples < 0) {
                ESP_LOGW(TAG, "opus decode failed: %d", samples);
                samples = 0;
            }
        }
        if (samples <= 0) {
            underruns++;
            memset(mono, 0, sizeof(mono));
        }
        /* The speaker codec runs on the shared stereo bus. */
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int16_t v = (i < samples) ? mono[i] : 0;
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_spk, stereo, sizeof(stereo));

        window_frames++;
        int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000) {
            ESP_LOGI(TAG, "playback %u frames, %u silent (no downlink)",
                     (unsigned)window_frames, (unsigned)underruns);
            window_frames = 0;
            underruns = 0;
            window_start = now;
        }
    }
    return NULL;
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
                    send_ctrl("ptt_down");
                }
            } else {
                const int64_t held = now - press_ms;
                if (held >= HOLD_MS) {
                    if (recording) {
                        recording = false;
                        s_talking = false;
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
static int s_battery_percent = -1;
static int s_battery_mv = 0;

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
static void battery_update(void)
{
    uint8_t adc = 0;
    if (pmu_read(0x30, &adc, 1) && !(adc & 0x01)) {
        pmu_write(0x30, (uint8_t)(adc | 0x01));
    }

    uint8_t raw[2] = {0};
    s_battery_mv = 0;
    if (pmu_read(0x34, raw, 2)) {
        s_battery_mv = ((raw[0] & 0x3F) << 8) | raw[1];
    }

    s_battery_percent = -1;
    uint8_t soc = 0;
    if (pmu_read(0xA4, &soc, 1) && soc <= 100) {
        s_battery_percent = soc;
    }
    if (s_battery_percent < 0 && s_battery_mv > 3000) {
        int pct = (s_battery_mv - 3500) * 100 / 700;
        s_battery_percent = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
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
static lv_obj_t *s_cap_mic;
static lv_obj_t *s_panel;

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

static lv_obj_t *glyph_label(lv_obj_t *parent, const char *symbol, int size, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, size >= 32 ? &lv_font_montserrat_32 : &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, symbol);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void close_panel(void)
{
    if (s_panel != NULL) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
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

static void on_volume_down(lv_event_t *e)
{
    (void)e;
    volume_apply(s_volume - 10, true);
    if (s_panel != NULL) {
        close_panel();
        on_sound(NULL);
    }
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
    volume_apply(s_volume + 10, true);
    if (s_panel != NULL) {
        close_panel();
        on_sound(NULL);
    }
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
    static char ssid_cache[40], vol_cache[8], bat_cache[8], mic_cache[10];
    char buffer[40];

    set_caption(s_cap_ssid, s_ssid_text, ssid_cache, sizeof(ssid_cache));

    snprintf(buffer, sizeof(buffer), "%d%%", s_volume);
    set_caption(s_cap_vol, buffer, vol_cache, sizeof(vol_cache));

    snprintf(buffer, sizeof(buffer), "%d%%", s_battery_percent < 0 ? 0 : s_battery_percent);

    set_caption(s_cap_bat, buffer, bat_cache, sizeof(bat_cache));

    set_caption(s_cap_mic, s_talking ? "ON AIR" : "idle", mic_cache, sizeof(mic_cache));
}

static void ui_start(void)
{
    if (display_start() == NULL) {
        ESP_LOGW(TAG, "display unavailable, running headless");
        return;
    }
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A1622), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x07101A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8FA8C4), 0);
    lv_label_set_text(title, "AUDIO BRIDGE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    static const struct {
        const char *symbol;
        uint32_t tint;
        lv_event_cb_t cb;
        lv_obj_t **caption;
    } tiles[4] = {
        { LV_SYMBOL_WIFI, 0x2C6E9B, on_wlan, &s_cap_ssid },
        { LV_SYMBOL_VOLUME_MAX, 0x6B4E9B, on_sound, &s_cap_vol },
        { LV_SYMBOL_BATTERY_3, 0x2E7D5B, on_info, &s_cap_bat },
        { LV_SYMBOL_AUDIO, 0x9B5E2C, on_info, &s_cap_mic },
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *tile = lv_obj_create(screen);
        lv_obj_set_size(tile, 176, 176);
        glass_style(tile, tiles[i].tint, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(tile, true);
        lv_obj_add_event_cb(tile, tiles[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_scrollable(tile, false);
        const int x = (i % 2 == 0) ? -96 : 96;
        const int y = (i < 2) ? -86 : 96;
        lv_obj_align(tile, LV_ALIGN_CENTER, x, y);

        glyph_label(tile, tiles[i].symbol, 32, -22);
        lv_obj_t *cap = lv_label_create(tile);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xE6EFF9), 0);
        /* Fixed width + centred text: a growing label leaves stale pixels inside the circle. */
        lv_obj_set_width(cap, 150);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(cap, "--");
        lv_obj_align(cap, LV_ALIGN_CENTER, 0, 42);
        *tiles[i].caption = cap;
    }

    refresh_captions();
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
        if (s_cap_ssid != NULL && bsp_display_lock(50) == ESP_OK) {
            refresh_captions();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
/* ------------------------------------------------------------------ network */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        s_link_up = false;
        esp_wifi_connect();
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
    }
}

static esp_err_t wifi_start(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
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


    ESP_LOGI(TAG, "bridge up: udp :%d, hold BOOT to talk", SERVER_PORT);
}

#include "board_audio.h"
#include "board_audio_rate.h"

#include <string.h>

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "board_audio";

enum {
    HARDWARE_MCLK_MULTIPLE = I2S_MCLK_MULTIPLE_384,
};

_Static_assert((int)BOARD_AUDIO_CAPTURE_CHANNELS == BOARD_AUDIO_RATE_CHANNELS &&
               (int)BOARD_AUDIO_CAPTURE_SAMPLE_BYTES == BOARD_AUDIO_PCM24_BYTES,
               "Audio converter requires packed PCM24 stereo");
_Static_assert(BOARD_AUDIO_CAPTURE_RATE == BOARD_AUDIO_VOICE_RATE * BOARD_AUDIO_RATE_RATIO,
               "Audio converter requires a 3:1 sample rate ratio");

typedef struct {
    const audio_codec_ctrl_if_t *control;
    const audio_codec_if_t *codec;
    esp_codec_dev_handle_t device;
} board_codec_t;

static struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    const audio_codec_data_if_t *data;
    const audio_codec_gpio_if_t *speaker_gpio;
    board_codec_t speaker;
    board_codec_t microphone;
    uint8_t *tx_pcm24;
    board_audio_decimator_t decimator;
    board_audio_interpolator_t interpolator;
    uint32_t last_read_overflows;
    uint32_t capture_epoch;
    bool playback_history;
} s_audio;

static DRAM_ATTR portMUX_TYPE s_rx_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR bool s_capture_active;
static DRAM_ATTR uint32_t s_rx_overflows;

static bool IRAM_ATTR on_i2s_rx_overflow(i2s_chan_handle_t channel,
                                       i2s_event_data_t *event, void *context)
{
    (void)channel;
    (void)event;
    (void)context;
    portENTER_CRITICAL_ISR(&s_rx_stats_lock);
    if (s_capture_active) {
        ++s_rx_overflows;
    }
    portEXIT_CRITICAL_ISR(&s_rx_stats_lock);
    return false;
}

static bool board_count_rx_overflows(bool active)
{
    portENTER_CRITICAL(&s_rx_stats_lock);
    const bool was_active = s_capture_active;
    s_capture_active = active;
    portEXIT_CRITICAL(&s_rx_stats_lock);
    return was_active;
}

int board_audio_set_capture_active(bool active)
{
    const bool was_counting = !active && board_count_rx_overflows(false);
    /* The IDF driver resets the RX FIFO and DMA queue on re-enable. The TX
     * channel is the clock master and stays running during this transition. */
    const int rc = s_audio.data->enable(s_audio.data, ESP_CODEC_DEV_TYPE_IN, active);
    if (rc != ESP_CODEC_DEV_OK) {
        /* The codec adapter reports an error for an already-enabled or
         * already-paused channel. Continue if the driver is in the requested state. */
        i2s_chan_info_t info;
        if (i2s_channel_get_info(s_audio.rx, &info) != ESP_OK || info.is_enabled != active) {
            if (!active) board_count_rx_overflows(was_counting);
            return rc;
        }
    }
    if (active) {
        s_audio.decimator = (board_audio_decimator_t){0};
        ++s_audio.capture_epoch;
        s_audio.last_read_overflows = board_audio_rx_overflows();
        board_count_rx_overflows(true);
    }
    return ESP_CODEC_DEV_OK;
}

uint32_t board_audio_rx_overflows(void)
{
    portENTER_CRITICAL(&s_rx_stats_lock);
    uint32_t count = s_rx_overflows;
    portEXIT_CRITICAL(&s_rx_stats_lock);
    return count;
}

int board_audio_read(uint8_t pcm24[BOARD_AUDIO_CAPTURE_BYTES], int16_t *vad_mono, uint32_t *capture_epoch)
{
    const int rc = esp_codec_dev_read(s_audio.microphone.device, pcm24, BOARD_AUDIO_CAPTURE_BYTES);
    const uint32_t overflows = board_audio_rx_overflows();
    if (rc != ESP_CODEC_DEV_OK || overflows != s_audio.last_read_overflows) {
        s_audio.decimator = (board_audio_decimator_t){0};
        ++s_audio.capture_epoch;
    }
    s_audio.last_read_overflows = overflows;
    *capture_epoch = s_audio.capture_epoch;
    if (rc == ESP_CODEC_DEV_OK && vad_mono != NULL) {
        board_audio_downsample_mono(&s_audio.decimator, pcm24, vad_mono, BOARD_AUDIO_FRAME_SAMPLES);
    }
    return rc;
}

int board_audio_write(const int16_t pcm[BOARD_AUDIO_FRAME_SAMPLES])
{
    if (pcm == NULL && !s_audio.playback_history) {
        memset(s_audio.tx_pcm24, 0, BOARD_AUDIO_CAPTURE_BYTES);
    } else {
        /* Interpolate the first silent block so the FIR can finish its tail. */
        static const int16_t silence[BOARD_AUDIO_FRAME_SAMPLES];
        board_audio_upsample(&s_audio.interpolator, pcm != NULL ? pcm : silence,
                             s_audio.tx_pcm24, BOARD_AUDIO_FRAME_SAMPLES);
    }
    s_audio.playback_history = pcm != NULL;
    const int rc = esp_codec_dev_write(s_audio.speaker.device, s_audio.tx_pcm24, BOARD_AUDIO_CAPTURE_BYTES);
    if (rc != ESP_CODEC_DEV_OK) {
        s_audio.interpolator = (board_audio_interpolator_t){0};
        s_audio.playback_history = false;
    }
    return rc;
}

static esp_err_t board_i2s_open(void)
{
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channels.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channels, &s_audio.tx, &s_audio.rx),
                        TAG, "allocate I2S channels");

    i2s_std_config_t stereo = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_CAPTURE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    /* 384 x Fs is divisible by the 48 BCLK cycles in each stereo frame. */
    stereo.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)HARDWARE_MCLK_MULTIPLE;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx, &stereo), TAG, "configure I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.tx), TAG, "enable I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx, &stereo), TAG, "configure I2S RX");

    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_i2s_rx_overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_audio.rx, &callbacks, NULL),
                        TAG, "register I2S RX overflow callback");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.rx), TAG, "enable I2S RX");
    return ESP_OK;
}

static bool board_i2s_running(i2s_chan_handle_t channel)
{
    i2s_chan_info_t info;
    return i2s_channel_get_info(channel, &info) == ESP_OK && info.is_enabled;
}

esp_err_t board_audio_open(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *microphone)
{
    ESP_RETURN_ON_FALSE(speaker && microphone && speaker != microphone,
                        ESP_ERR_INVALID_ARG, TAG, "invalid board audio configuration");
    ESP_RETURN_ON_FALSE(!s_audio.tx && !s_audio.rx, ESP_ERR_INVALID_STATE, TAG, "audio already open");
    *speaker = NULL;
    *microphone = NULL;
    portENTER_CRITICAL(&s_rx_stats_lock);
    s_capture_active = false;
    s_rx_overflows = 0;
    portEXIT_CRITICAL(&s_rx_stats_lock);

    /* The driver copies between application buffers and its own DMA memory. */
    esp_err_t ret = ESP_OK;
    s_audio.tx_pcm24 = heap_caps_malloc(BOARD_AUDIO_CAPTURE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(s_audio.tx_pcm24, ESP_ERR_NO_MEM, fail, TAG, "allocate PCM24 playback buffer");
    s_audio.decimator = (board_audio_decimator_t){0};
    s_audio.interpolator = (board_audio_interpolator_t){0};
    s_audio.playback_history = false;

    ESP_GOTO_ON_ERROR(board_i2s_open(), fail, TAG, "open I2S bus");
    audio_codec_i2s_cfg_t data = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_audio.rx,
        .tx_handle = s_audio.tx,
    };
    s_audio.data = audio_codec_new_i2s_data(&data);
    ESP_GOTO_ON_FALSE(s_audio.data, ESP_ERR_NO_MEM, fail, TAG, "create codec I2S interface");

    ESP_GOTO_ON_ERROR(bsp_i2c_init(), fail, TAG, "initialize shared board I2C bus");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_GOTO_ON_FALSE(bus, ESP_ERR_INVALID_STATE, fail, TAG, "get shared board I2C bus");

    audio_codec_i2c_cfg_t control = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    s_audio.speaker.control = audio_codec_new_i2c_ctrl(&control);
    ESP_GOTO_ON_FALSE(s_audio.speaker.control, ESP_FAIL, fail, TAG, "create ES8311 I2C interface");
    s_audio.speaker_gpio = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(s_audio.speaker_gpio, ESP_ERR_NO_MEM, fail, TAG, "create speaker GPIO interface");
    es8311_codec_cfg_t output = {
        .ctrl_if = s_audio.speaker.control,
        .gpio_if = s_audio.speaker_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .mclk_div = HARDWARE_MCLK_MULTIPLE,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    s_audio.speaker.codec = es8311_codec_new(&output);
    ESP_GOTO_ON_FALSE(s_audio.speaker.codec, ESP_FAIL, fail, TAG, "initialize ES8311 speaker");

    control.addr = ES7210_CODEC_DEFAULT_ADDR;
    s_audio.microphone.control = audio_codec_new_i2c_ctrl(&control);
    ESP_GOTO_ON_FALSE(s_audio.microphone.control, ESP_FAIL, fail, TAG, "create ES7210 I2C interface");
    es7210_codec_cfg_t input = {.ctrl_if = s_audio.microphone.control};
    s_audio.microphone.codec = es7210_codec_new(&input);
    ESP_GOTO_ON_FALSE(s_audio.microphone.codec, ESP_FAIL, fail, TAG, "initialize ES7210 microphone");

    esp_codec_dev_cfg_t device = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_audio.speaker.codec,
        .data_if = s_audio.data,
    };
    s_audio.speaker.device = esp_codec_dev_new(&device);
    ESP_GOTO_ON_FALSE(s_audio.speaker.device, ESP_ERR_NO_MEM, fail, TAG, "create speaker device");
    device.dev_type = ESP_CODEC_DEV_TYPE_IN;
    device.codec_if = s_audio.microphone.codec;
    s_audio.microphone.device = esp_codec_dev_new(&device);
    ESP_GOTO_ON_FALSE(s_audio.microphone.device, ESP_ERR_NO_MEM, fail, TAG, "create microphone device");

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_24BIT,
        .channel = BOARD_AUDIO_CAPTURE_CHANNELS,
        .sample_rate = BOARD_AUDIO_CAPTURE_RATE,
        .mclk_multiple = HARDWARE_MCLK_MULTIPLE,
    };
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(s_audio.speaker.device, &format), fail, TAG, "open speaker");

    // Raise the listening range while keeping fine control at low settings.
    esp_codec_dev_vol_map_t speaker_volume_points[] = {
        {.vol = 0, .db_value = -50.0f},
        {.vol = 20, .db_value = -30.0f},
        {.vol = 50, .db_value = -12.0f},
        {.vol = 100, .db_value = 0.0f},
    };
    esp_codec_dev_vol_curve_t speaker_volume_curve = {
        .vol_map = speaker_volume_points,
        .count = sizeof(speaker_volume_points) / sizeof(speaker_volume_points[0]),
    };
    const int volume_curve_rc = esp_codec_dev_set_vol_curve(s_audio.speaker.device, &speaker_volume_curve);
    if (volume_curve_rc == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "speaker volume curve installed");
    } else {
        ESP_LOGW(TAG, "using default speaker volume curve: rc=%d", volume_curve_rc);
    }

    ESP_GOTO_ON_ERROR(esp_codec_dev_open(s_audio.microphone.device, &format), fail, TAG, "open microphone");
    ESP_GOTO_ON_FALSE(board_i2s_running(s_audio.tx) && board_i2s_running(s_audio.rx),
                      ESP_ERR_INVALID_STATE, fail, TAG, "I2S channel stopped during codec setup");
    ESP_GOTO_ON_FALSE(board_audio_set_capture_active(false) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "pause RX DMA until capture starts");
    *speaker = s_audio.speaker.device;
    *microphone = s_audio.microphone.device;
    ESP_LOGI(TAG, "I2S: %d Hz, %d-bit stereo; VAD/playback PCM: %d Hz, 16-bit mono",
             BOARD_AUDIO_CAPTURE_RATE, format.bits_per_sample, BOARD_AUDIO_VOICE_RATE);
    ESP_LOGI(TAG, "RX DMA pauses when capture is idle");
    return ESP_OK;

fail:
    board_audio_close();
    return ret;
}

static void board_codec_close(board_codec_t *codec)
{
    esp_codec_dev_delete(codec->device);
    if (codec->codec) {
        audio_codec_delete_codec_if(codec->codec);
    }
    if (codec->control) {
        audio_codec_delete_ctrl_if(codec->control);
    }
    *codec = (board_codec_t){0};
}

static void board_i2s_close(i2s_chan_handle_t *channel)
{
    if (*channel == NULL) {
        return;
    }
    if (board_i2s_running(*channel)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(*channel));
    }
    esp_err_t ret = i2s_del_channel(*channel);
    if (ret == ESP_OK) {
        *channel = NULL;
    } else {
        ESP_LOGE(TAG, "delete I2S channel: %s", esp_err_to_name(ret));
    }
}

void board_audio_close(void)
{
    board_count_rx_overflows(false);
    board_codec_close(&s_audio.microphone);
    board_codec_close(&s_audio.speaker);
    if (s_audio.speaker_gpio) {
        audio_codec_delete_gpio_if(s_audio.speaker_gpio);
        s_audio.speaker_gpio = NULL;
    }
    if (s_audio.data) {
        audio_codec_delete_data_if(s_audio.data);
        s_audio.data = NULL;
    }
    board_i2s_close(&s_audio.rx);
    board_i2s_close(&s_audio.tx);
    heap_caps_free(s_audio.tx_pcm24);
    s_audio.tx_pcm24 = NULL;
}

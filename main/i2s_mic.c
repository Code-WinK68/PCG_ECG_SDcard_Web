#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "driver/i2s_std.h"

#include "app_config.h"
#include "i2s_mic.h"

static const char *TAG = "I2S_MIC";
static i2s_chan_handle_t s_rx_handle = NULL;
static bool s_started = false;

esp_err_t i2s_mic_init(void)
{
    if (s_rx_handle != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = PCG_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = PCG_DMA_BUF_LEN;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCG_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DATA_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* 24-bit microphone payload inside 32-bit slots: use a multiple of 3. */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        (void)i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "I2S ready: %u Hz, mono-left, 32-bit slot / 24-bit payload",
             PCG_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t i2s_mic_start(void)
{
    if (s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    const esp_err_t ret = i2s_channel_enable(s_rx_handle);
    if (ret == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "I2S stream started");
    } else {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t i2s_mic_stop(void)
{
    if (s_rx_handle == NULL || !s_started) {
        return ESP_OK;
    }

    const esp_err_t ret = i2s_channel_disable(s_rx_handle);
    if (ret == ESP_OK) {
        s_started = false;
        ESP_LOGI(TAG, "I2S stream stopped");
    } else {
        ESP_LOGW(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t i2s_mic_read_samples(int32_t *out_samples,
                                size_t sample_count,
                                size_t *out_count,
                                uint32_t timeout_ms)
{
    if (s_rx_handle == NULL || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_samples == NULL || out_count == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_read = 0U;
    const size_t bytes_requested = sample_count * sizeof(int32_t);

    /* ESP-IDF 5.x expects timeout_ms directly, not FreeRTOS ticks. */
    const esp_err_t ret = i2s_channel_read(s_rx_handle,
                                           out_samples,
                                           bytes_requested,
                                           &bytes_read,
                                           timeout_ms);

    *out_count = bytes_read / sizeof(int32_t);
    if (*out_count > sample_count) {
        *out_count = sample_count;
    }

    /* ICS-43434 is MSB-aligned: [31:8] data, [7:0] padding. */
    for (size_t i = 0U; i < *out_count; ++i) {
        out_samples[i] >>= 8;
    }

    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "I2S timeout: %u/%u samples", (unsigned)*out_count, (unsigned)sample_count);
        return ESP_OK; /* Caller marks missing samples invalid but keeps timeline. */
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t i2s_mic_deinit(void)
{
    if (s_rx_handle == NULL) {
        return ESP_OK;
    }

    (void)i2s_mic_stop();
    const esp_err_t ret = i2s_del_channel(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_del_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_rx_handle = NULL;
    s_started = false;
    ESP_LOGI(TAG, "I2S deinitialized");
    return ESP_OK;
}

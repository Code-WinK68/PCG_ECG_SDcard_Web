#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

#include "app_config.h"
#include "ecg_adc.h"

static const char *TAG = "ECG_ADC";
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

esp_err_t ecg_adc_init(void)
{
    if (s_adc_handle != NULL) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ECG_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ECG_ADC_ATTEN,
        .bitwidth = ECG_ADC_BITWIDTH,
    };

    ret = adc_oneshot_config_channel(s_adc_handle, ECG_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    /* AD8232 LO+ and LO- are actively driven digital status outputs.
     * Do not enable ESP32 internal pulls: the AD8232 board must drive them.
     */
    const gpio_config_t lead_off_gpio_cfg = {
        .pin_bit_mask = (1ULL << ECG_LO_PLUS_GPIO) | (1ULL << ECG_LO_MINUS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&lead_off_gpio_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Lead-off GPIO configuration failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG,
             "ECG ready: ADC GPIO%d/ADC1_CH0 | LO+=GPIO%d | LO-=GPIO%d",
             ECG_ADC_GPIO,
             ECG_LO_PLUS_GPIO,
             ECG_LO_MINUS_GPIO);
    return ESP_OK;
}

esp_err_t ecg_adc_read_raw(uint16_t *out_raw)
{
    if (s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, ECG_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_raw = (uint16_t)raw;
    return ESP_OK;
}

esp_err_t ecg_adc_read_lead_status(uint8_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = ECG_LEAD_STATUS_NONE;
    if (gpio_get_level(ECG_LO_PLUS_GPIO) != 0) {
        status |= ECG_LEAD_STATUS_LO_PLUS;
    }
    if (gpio_get_level(ECG_LO_MINUS_GPIO) != 0) {
        status |= ECG_LEAD_STATUS_LO_MINUS;
    }

    *out_status = status;
    return ESP_OK;
}

esp_err_t ecg_adc_deinit(void)
{
    if (s_adc_handle == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = adc_oneshot_del_unit(s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_del_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_adc_handle = NULL;
    ESP_LOGI(TAG, "ECG ADC deinitialized");
    return ESP_OK;
}

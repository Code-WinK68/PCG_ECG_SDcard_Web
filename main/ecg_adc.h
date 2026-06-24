#ifndef ECG_ADC_H
#define ECG_ADC_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t ecg_adc_init(void);
esp_err_t ecg_adc_read_raw(uint16_t *out_raw);

/* Read AD8232 lead-off state sampled from LO+ and LO- GPIO inputs. */
esp_err_t ecg_adc_read_lead_status(uint8_t *out_status);

esp_err_t ecg_adc_deinit(void);

#endif /* ECG_ADC_H */

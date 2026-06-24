#ifndef I2S_MIC_H
#define I2S_MIC_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Configure I2S RX but do not start BCLK/WS yet. */
esp_err_t i2s_mic_init(void);

/* Start/stop the PCG master clock. */
esp_err_t i2s_mic_start(void);
esp_err_t i2s_mic_stop(void);

/* Read an arbitrary positive number of 32-bit I2S samples.
 * For this project Core 0 reads exactly 40 samples per call.
 * timeout_ms is in milliseconds, exactly as expected by ESP-IDF i2s_channel_read(). */
esp_err_t i2s_mic_read_samples(int32_t *out_samples,
                                size_t sample_count,
                                size_t *out_count,
                                uint32_t timeout_ms);

esp_err_t i2s_mic_deinit(void);

#endif /* I2S_MIC_H */

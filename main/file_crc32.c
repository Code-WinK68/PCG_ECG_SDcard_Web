#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "file_crc32.h"

static const char *TAG = "FILE_CRC32";

static uint32_t crc32_update_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned int bit = 0; bit < 8U; ++bit) {
        crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
    }
    return crc;
}

esp_err_t file_crc32_compute(const char *path, uint32_t *out_crc32, uint64_t *out_size_bytes)
{
    if (path == NULL || out_crc32 == NULL || out_size_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    uint8_t buffer[1024];
    uint32_t crc = 0xFFFFFFFFU;
    uint64_t total = 0U;

    while (true) {
        const size_t n = fread(buffer, 1, sizeof(buffer), file);
        for (size_t i = 0; i < n; ++i) {
            crc = crc32_update_byte(crc, buffer[i]);
        }
        total += n;

        if (n < sizeof(buffer)) {
            if (ferror(file)) {
                ESP_LOGE(TAG, "Read failure during CRC32 of %s", path);
                fclose(file);
                return ESP_FAIL;
            }
            break;
        }
    }

    fclose(file);
    *out_crc32 = crc ^ 0xFFFFFFFFU;
    *out_size_bytes = total;

    ESP_LOGI(TAG, "CRC32 file=%s size=%llu crc=0x%08lx",
             path,
             (unsigned long long)total,
             (unsigned long)*out_crc32);
    return ESP_OK;
}

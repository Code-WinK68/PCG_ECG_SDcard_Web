#ifndef FILE_CRC32_H
#define FILE_CRC32_H

#include <stdint.h>
#include "esp_err.h"

/* Standard CRC-32/ISO-HDLC: poly 0x04C11DB7, reflected implementation. */
esp_err_t file_crc32_compute(const char *path, uint32_t *out_crc32, uint64_t *out_size_bytes);

#endif /* FILE_CRC32_H */

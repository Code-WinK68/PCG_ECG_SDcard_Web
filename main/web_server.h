#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Immutable session metadata copied when the local web phase begins. */
typedef struct {
    char     filename[64];
    char     pcg_raw_filename[64];
    char     ecg_raw_filename[64];
    uint64_t total_pcg_samples;
    uint32_t pcg_blocks_captured;
    uint32_t pcg_blocks_dropped;
    uint32_t pcg_short_blocks;
    uint32_t ecg_samples_captured;
    uint32_t ecg_samples_dropped;
    uint32_t ecg_samples_missing_in_merge;
    uint32_t sync_tokens_dropped;
    uint32_t ecg_late_notifications;
    uint32_t pcg_raw_frame_gaps;
    uint64_t pcg_raw_gap_samples;
    uint32_t ecg_lead_off_samples;
    uint32_t ecg_lo_plus_samples;
    uint32_t ecg_lo_minus_samples;
    uint32_t sd_write_errors;
    int64_t  ecg_first_offset_us;
    uint32_t file_crc32;
    uint64_t file_size_bytes;
    bool     crc32_valid;
    bool     capture_valid;
} web_session_info_t;

/*
 * Starts ESP32 SoftAP + local HTTP server. The caller must keep the SD card
 * mounted and the CSV file closed, because handlers read the completed file.
 */
esp_err_t web_server_start(const web_session_info_t *session);

#endif /* WEB_SERVER_H */

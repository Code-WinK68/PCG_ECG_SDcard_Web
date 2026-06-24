#ifndef SD_WRITER_H
#define SD_WRITER_H

#include <stdint.h>

#include "esp_err.h"
#include "app_config.h"

typedef struct {
    uint32_t pcg_frames_written;
    uint32_t pcg_frame_gaps;
    uint64_t pcg_samples_missing_from_raw;
    uint32_t ecg_records_mapped;
    uint32_t ecg_records_unmapped;
    uint32_t ecg_slots_missing_in_csv;
    uint64_t csv_samples_written;
} sd_conversion_stats_t;

/* Mount FAT32 and reserve a unique short-name session: Pxxx.BIN, Exxx.BIN, Cxxx.CSV. */
esp_err_t sd_writer_init(void);

/* Preallocate both raw binary files before the start barrier. */
esp_err_t sd_writer_begin_raw_record(uint64_t expected_slots);
esp_err_t sd_writer_write_pcg_frame(const pcg_frame_t *frame);
esp_err_t sd_writer_write_ecg_batch(const ecg_batch_t *batch);

/* Flush, write actual-record counts back into raw headers, then close raw files. */
esp_err_t sd_writer_close_raw_record(int64_t pcg_start_time_us);

/* Offline phase: read actual raw records only and generate Cxxx.CSV. */
esp_err_t sd_writer_convert_raw_to_csv(sd_conversion_stats_t *out_stats);

const char *sd_writer_get_filename(void);
const char *sd_writer_get_pcg_raw_filename(void);
const char *sd_writer_get_ecg_raw_filename(void);

/* SD remains mounted while the local HTTP server serves files. */
esp_err_t sd_writer_deinit(void);

#endif /* SD_WRITER_H */

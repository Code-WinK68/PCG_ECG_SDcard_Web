#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "app_config.h"
#include "sd_writer.h"

static const char *TAG = "SD_WRITER";

#define PCG_RAW_MAGIC       0x50434736UL /* PCG6 */
#define ECG_RAW_MAGIC       0x45434736UL /* ECG6 */
#define RAW_FORMAT_VERSION  2U

/* Header fields are updated only after capture has stopped. The preallocated
 * tail is ignored because conversion reads actual_* counts, never file EOF. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t pcg_sample_rate;
    uint32_t frame_samples;
    uint64_t expected_slots;
    uint32_t expected_frames;
    uint32_t actual_frames;
    uint64_t actual_valid_samples;
    int64_t  pcg_start_time_us;
} pcg_raw_file_header_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t ecg_sample_rate;
    uint32_t pcg_per_ecg_slot;
    uint64_t expected_slots;
    uint32_t actual_records;
    uint32_t reserved;
    int64_t  pcg_start_time_us;
} ecg_raw_file_header_t;

typedef struct {
    uint64_t first_slot_id;
    uint64_t first_pcg_index;
    uint16_t valid_sample_count;
    uint16_t reserved;
    uint32_t sequence;
} pcg_raw_frame_header_t;

static sdmmc_card_t *s_card = NULL;
static FILE *s_pcg_raw_file = NULL;
static FILE *s_ecg_raw_file = NULL;
static char s_pcg_raw_filename[64];
static char s_ecg_raw_filename[64];
static char s_csv_filename[64];

/* All large work buffers are static. They never consume task stack. */
static uint8_t s_pcg_io_buffer[SD_PCG_IO_BUFFER_BYTES];
static uint8_t s_ecg_io_buffer[SD_ECG_IO_BUFFER_BYTES];
static char s_csv_io_buffer[SD_CSV_IO_BUFFER_BYTES];
static char s_csv_text_buffer[SD_CSV_TEXT_BUFFER_BYTES];
static int32_t s_convert_pcg_samples[PCG_FRAME_SAMPLES];
static uint8_t s_convert_pcg_valid[PCG_FRAME_SAMPLES];

static pcg_raw_file_header_t s_pcg_header;
static ecg_raw_file_header_t s_ecg_header;

static esp_err_t write_exact(FILE *file, const void *data, size_t bytes, const char *what)
{
    if (file == NULL || data == NULL || bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fwrite(data, 1U, bytes, file) != bytes) {
        ESP_LOGE(TAG, "Write failed for %s | errno=%d (%s)",
                 what, errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t fail_file_open(const char *path)
{
    const int saved_errno = errno;
    ESP_LOGE(TAG, "Cannot open %s | errno=%d (%s)",
             path, saved_errno, strerror(saved_errno));
    return ESP_FAIL;
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

static esp_err_t choose_next_session(void)
{
    for (unsigned int id = 0U; id < 1000U; ++id) {
        const int a = snprintf(s_pcg_raw_filename, sizeof(s_pcg_raw_filename),
                               "%s%03u.BIN", SD_PCG_RAW_PREFIX, id);
        const int b = snprintf(s_ecg_raw_filename, sizeof(s_ecg_raw_filename),
                               "%s%03u.BIN", SD_ECG_RAW_PREFIX, id);
        const int c = snprintf(s_csv_filename, sizeof(s_csv_filename),
                               "%s%03u.CSV", SD_CSV_PREFIX, id);
        if (a < 0 || b < 0 || c < 0 ||
            (size_t)a >= sizeof(s_pcg_raw_filename) ||
            (size_t)b >= sizeof(s_ecg_raw_filename) ||
            (size_t)c >= sizeof(s_csv_filename)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!file_exists(s_pcg_raw_filename) &&
            !file_exists(s_ecg_raw_filename) &&
            !file_exists(s_csv_filename)) {
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No free session index in 000..999");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t preallocate_file(FILE *file, uint64_t bytes, const char *label)
{
#if SD_PREALLOCATE_RAW_FILES
    if (file == NULL || bytes == 0U || bytes > (uint64_t)LONG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(file, (long)(bytes - 1U), SEEK_SET) != 0 ||
        fputc(0, file) == EOF ||
        fflush(file) != 0 ||
        fseek(file, 0L, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Preallocation failed for %s | errno=%d (%s)",
                 label, errno, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Preallocated %s: %" PRIu64 " bytes", label, bytes);
#else
    (void)file;
    (void)bytes;
    (void)label;
#endif
    return ESP_OK;
}

static uint64_t expected_pcg_raw_bytes(void)
{
    const uint64_t one_frame = (uint64_t)sizeof(pcg_raw_frame_header_t) +
                               (uint64_t)PCG_FRAME_SAMPLES * sizeof(int32_t) +
                               (uint64_t)PCG_FRAME_SAMPLES * sizeof(uint8_t);
    return (uint64_t)sizeof(pcg_raw_file_header_t) +
           (uint64_t)RECORD_TOTAL_FRAMES * one_frame;
}

static uint64_t expected_ecg_raw_bytes(void)
{
    return (uint64_t)sizeof(ecg_raw_file_header_t) +
           (uint64_t)RECORD_TOTAL_SLOTS * sizeof(ecg_sample_t);
}

static esp_err_t write_headers_at_start(void)
{
    if (write_exact(s_pcg_raw_file, &s_pcg_header, sizeof(s_pcg_header),
                    "PCG raw header") != ESP_OK ||
        write_exact(s_ecg_raw_file, &s_ecg_header, sizeof(s_ecg_header),
                    "ECG raw header") != ESP_OK) {
        return ESP_FAIL;
    }

    if (fseek(s_pcg_raw_file, (long)sizeof(s_pcg_header), SEEK_SET) != 0 ||
        fseek(s_ecg_raw_file, (long)sizeof(s_ecg_header), SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Cannot seek to raw payload start | errno=%d (%s)",
                 errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t update_headers_before_close(int64_t pcg_start_time_us)
{
    s_pcg_header.pcg_start_time_us = pcg_start_time_us;
    s_ecg_header.pcg_start_time_us = pcg_start_time_us;

    if (fflush(s_pcg_raw_file) != 0 || fflush(s_ecg_raw_file) != 0 ||
        fseek(s_pcg_raw_file, 0L, SEEK_SET) != 0 ||
        fseek(s_ecg_raw_file, 0L, SEEK_SET) != 0 ||
        write_exact(s_pcg_raw_file, &s_pcg_header, sizeof(s_pcg_header),
                    "PCG final header") != ESP_OK ||
        write_exact(s_ecg_raw_file, &s_ecg_header, sizeof(s_ecg_header),
                    "ECG final header") != ESP_OK ||
        fflush(s_pcg_raw_file) != 0 || fflush(s_ecg_raw_file) != 0) {
        ESP_LOGE(TAG, "Cannot finalize raw headers | errno=%d (%s)",
                 errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_writer_init(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16U * 1024U,
    };
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_MAX_TRANSFER_BYTES,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_PIN;
    slot_cfg.host_id = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        (void)spi_bus_free(SD_SPI_HOST);
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ret = choose_next_session();
    if (ret != ESP_OK) {
        (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        (void)spi_bus_free(SD_SPI_HOST);
        return ret;
    }

    ESP_LOGI(TAG, "SD ready: PCG=%s ECG=%s CSV=%s",
             s_pcg_raw_filename, s_ecg_raw_filename, s_csv_filename);
    return ESP_OK;
}

esp_err_t sd_writer_begin_raw_record(uint64_t expected_slots)
{
    if (s_card == NULL || s_pcg_raw_file != NULL || s_ecg_raw_file != NULL ||
        expected_slots != RECORD_TOTAL_SLOTS) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_pcg_header, 0, sizeof(s_pcg_header));
    memset(&s_ecg_header, 0, sizeof(s_ecg_header));
    s_pcg_header.magic = PCG_RAW_MAGIC;
    s_pcg_header.version = RAW_FORMAT_VERSION;
    s_pcg_header.header_size = sizeof(s_pcg_header);
    s_pcg_header.pcg_sample_rate = PCG_SAMPLE_RATE;
    s_pcg_header.frame_samples = PCG_FRAME_SAMPLES;
    s_pcg_header.expected_slots = expected_slots;
    s_pcg_header.expected_frames = RECORD_TOTAL_FRAMES;

    s_ecg_header.magic = ECG_RAW_MAGIC;
    s_ecg_header.version = RAW_FORMAT_VERSION;
    s_ecg_header.header_size = sizeof(s_ecg_header);
    s_ecg_header.ecg_sample_rate = ECG_SAMPLE_RATE;
    s_ecg_header.pcg_per_ecg_slot = PCG_PER_ECG_SLOT;
    s_ecg_header.expected_slots = expected_slots;

    s_pcg_raw_file = fopen(s_pcg_raw_filename, "wb+");
    if (s_pcg_raw_file == NULL) {
        return fail_file_open(s_pcg_raw_filename);
    }
    if (setvbuf(s_pcg_raw_file, (char *)s_pcg_io_buffer, _IOFBF,
                sizeof(s_pcg_io_buffer)) != 0) {
        ESP_LOGW(TAG, "PCG file buffer not accepted; continuing");
    }

    s_ecg_raw_file = fopen(s_ecg_raw_filename, "wb+");
    if (s_ecg_raw_file == NULL) {
        (void)fail_file_open(s_ecg_raw_filename);
        fclose(s_pcg_raw_file);
        s_pcg_raw_file = NULL;
        return ESP_FAIL;
    }
    if (setvbuf(s_ecg_raw_file, (char *)s_ecg_io_buffer, _IOFBF,
                sizeof(s_ecg_io_buffer)) != 0) {
        ESP_LOGW(TAG, "ECG file buffer not accepted; continuing");
    }

    if (preallocate_file(s_pcg_raw_file, expected_pcg_raw_bytes(), "PCG raw") != ESP_OK ||
        preallocate_file(s_ecg_raw_file, expected_ecg_raw_bytes(), "ECG raw") != ESP_OK ||
        write_headers_at_start() != ESP_OK) {
        fclose(s_pcg_raw_file);
        fclose(s_ecg_raw_file);
        s_pcg_raw_file = NULL;
        s_ecg_raw_file = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Raw files opened and preallocated before I2S start");
    return ESP_OK;
}

esp_err_t sd_writer_write_pcg_frame(const pcg_frame_t *frame)
{
    if (s_pcg_raw_file == NULL || frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const pcg_raw_frame_header_t header = {
        .first_slot_id = frame->first_slot_id,
        .first_pcg_index = frame->first_pcg_index,
        .valid_sample_count = frame->valid_sample_count,
        .reserved = 0U,
        .sequence = s_pcg_header.actual_frames,
    };

    if (write_exact(s_pcg_raw_file, &header, sizeof(header), "PCG frame header") != ESP_OK ||
        write_exact(s_pcg_raw_file, frame->pcg_samples,
                    sizeof(frame->pcg_samples), "PCG frame samples") != ESP_OK ||
        write_exact(s_pcg_raw_file, frame->pcg_valid,
                    sizeof(frame->pcg_valid), "PCG validity") != ESP_OK) {
        return ESP_FAIL;
    }

    ++s_pcg_header.actual_frames;
    s_pcg_header.actual_valid_samples += frame->valid_sample_count;

    /* No explicit fflush in the capture phase. The files were already
     * preallocated; stdio flushes only when its own buffer fills. */
    if (SD_FLUSH_EVERY_FRAMES != 0U &&
        (s_pcg_header.actual_frames % SD_FLUSH_EVERY_FRAMES) == 0U &&
        fflush(s_pcg_raw_file) != 0) {
        ESP_LOGE(TAG, "PCG raw fflush failed | errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_writer_write_ecg_batch(const ecg_batch_t *batch)
{
    if (s_ecg_raw_file == NULL || batch == NULL ||
        batch->count > SYNC_SLOTS_PER_FRAME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (batch->count == 0U) {
        return ESP_OK;
    }

    const size_t bytes = (size_t)batch->count * sizeof(batch->samples[0]);
    if (write_exact(s_ecg_raw_file, batch->samples, bytes, "ECG batch") != ESP_OK) {
        return ESP_FAIL;
    }

    s_ecg_header.actual_records += batch->count;
    if (SD_FLUSH_EVERY_ECG_BATCHES != 0U &&
        ((s_ecg_header.actual_records / SYNC_SLOTS_PER_FRAME) %
         SD_FLUSH_EVERY_ECG_BATCHES) == 0U &&
        fflush(s_ecg_raw_file) != 0) {
        ESP_LOGE(TAG, "ECG raw fflush failed | errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_writer_close_raw_record(int64_t pcg_start_time_us)
{
    esp_err_t ret = ESP_OK;
    if (s_pcg_raw_file != NULL && s_ecg_raw_file != NULL) {
        ret = update_headers_before_close(pcg_start_time_us);
    }

    if (s_pcg_raw_file != NULL) {
        if (fclose(s_pcg_raw_file) != 0) {
            ESP_LOGE(TAG, "Cannot close PCG raw file");
            ret = ESP_FAIL;
        }
        s_pcg_raw_file = NULL;
    }
    if (s_ecg_raw_file != NULL) {
        if (fclose(s_ecg_raw_file) != 0) {
            ESP_LOGE(TAG, "Cannot close ECG raw file");
            ret = ESP_FAIL;
        }
        s_ecg_raw_file = NULL;
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "Raw files closed: PCG frames=%" PRIu32 " valid=%" PRIu64
                 " | ECG records=%" PRIu32,
                 s_pcg_header.actual_frames,
                 s_pcg_header.actual_valid_samples,
                 s_ecg_header.actual_records);
    }
    return ret;
}

static bool read_next_ecg(FILE *ecg_file,
                          uint32_t *remaining,
                          ecg_sample_t *record,
                          bool *has_record)
{
    if (remaining == NULL || record == NULL || has_record == NULL) {
        return false;
    }
    if (*remaining == 0U) {
        *has_record = false;
        return true;
    }
    if (fread(record, 1U, sizeof(*record), ecg_file) != sizeof(*record)) {
        *has_record = false;
        return false;
    }
    --(*remaining);
    *has_record = true;
    return true;
}

static uint8_t build_ecg_flags(const ecg_sample_t *record)
{
    if (record->valid == 0U) {
        return CSV_FLAG_ECG_MISSING;
    }

    uint8_t flags = CSV_FLAG_ECG_VALID;
    if ((record->lead_status & ECG_LEAD_STATUS_ANY) != 0U) {
        flags |= CSV_FLAG_ECG_LEAD_OFF;
    }
    if ((record->lead_status & ECG_LEAD_STATUS_LO_PLUS) != 0U) {
        flags |= CSV_FLAG_ECG_LO_PLUS;
    }
    if ((record->lead_status & ECG_LEAD_STATUS_LO_MINUS) != 0U) {
        flags |= CSV_FLAG_ECG_LO_MINUS;
    }
    return flags;
}

esp_err_t sd_writer_convert_raw_to_csv(sd_conversion_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_stats, 0, sizeof(*out_stats));

    FILE *pcg_file = fopen(s_pcg_raw_filename, "rb");
    if (pcg_file == NULL) {
        return fail_file_open(s_pcg_raw_filename);
    }
    FILE *ecg_file = fopen(s_ecg_raw_filename, "rb");
    if (ecg_file == NULL) {
        (void)fail_file_open(s_ecg_raw_filename);
        fclose(pcg_file);
        return ESP_FAIL;
    }

    pcg_raw_file_header_t pcg_header = { 0 };
    ecg_raw_file_header_t ecg_header = { 0 };
    const bool valid_headers =
        fread(&pcg_header, 1U, sizeof(pcg_header), pcg_file) == sizeof(pcg_header) &&
        fread(&ecg_header, 1U, sizeof(ecg_header), ecg_file) == sizeof(ecg_header) &&
        pcg_header.magic == PCG_RAW_MAGIC &&
        ecg_header.magic == ECG_RAW_MAGIC &&
        pcg_header.version == RAW_FORMAT_VERSION &&
        ecg_header.version == RAW_FORMAT_VERSION &&
        pcg_header.pcg_sample_rate == PCG_SAMPLE_RATE &&
        pcg_header.frame_samples == PCG_FRAME_SAMPLES &&
        pcg_header.actual_frames <= pcg_header.expected_frames &&
        ecg_header.ecg_sample_rate == ECG_SAMPLE_RATE &&
        ecg_header.pcg_per_ecg_slot == PCG_PER_ECG_SLOT &&
        ecg_header.actual_records <= ecg_header.expected_slots;
    if (!valid_headers) {
        ESP_LOGE(TAG, "Raw header validation failed");
        fclose(pcg_file);
        fclose(ecg_file);
        return ESP_FAIL;
    }

    FILE *csv_file = fopen(s_csv_filename, "w");
    if (csv_file == NULL) {
        (void)fail_file_open(s_csv_filename);
        fclose(pcg_file);
        fclose(ecg_file);
        return ESP_FAIL;
    }
    (void)setvbuf(csv_file, s_csv_io_buffer, _IOFBF, sizeof(s_csv_io_buffer));
    if (fprintf(csv_file,
                "sample_index,timestamp_us,ecg_raw,pcg_mono,valid_flags\n") < 0) {
        fclose(csv_file);
        fclose(pcg_file);
        fclose(ecg_file);
        return ESP_FAIL;
    }

    uint32_t ecg_remaining = ecg_header.actual_records;
    ecg_sample_t next_ecg = { 0 };
    bool has_ecg = false;
    if (!read_next_ecg(ecg_file, &ecg_remaining, &next_ecg, &has_ecg)) {
        ESP_LOGE(TAG, "Cannot read first ECG raw record");
        fclose(csv_file);
        fclose(pcg_file);
        fclose(ecg_file);
        return ESP_FAIL;
    }

    uint64_t expected_next_index = 0U;
    for (uint32_t frame_no = 0U; frame_no < pcg_header.actual_frames; ++frame_no) {
        pcg_raw_frame_header_t frame_header = { 0 };
        if (fread(&frame_header, 1U, sizeof(frame_header), pcg_file) != sizeof(frame_header) ||
            fread(s_convert_pcg_samples, 1U, sizeof(s_convert_pcg_samples), pcg_file) !=
                sizeof(s_convert_pcg_samples) ||
            fread(s_convert_pcg_valid, 1U, sizeof(s_convert_pcg_valid), pcg_file) !=
                sizeof(s_convert_pcg_valid)) {
            ESP_LOGE(TAG, "Truncated PCG raw record at frame=%" PRIu32, frame_no);
            fclose(csv_file);
            fclose(pcg_file);
            fclose(ecg_file);
            return ESP_FAIL;
        }

        if (frame_header.first_pcg_index != expected_next_index) {
            ++out_stats->pcg_frame_gaps;
            if (frame_header.first_pcg_index > expected_next_index) {
                out_stats->pcg_samples_missing_from_raw +=
                    frame_header.first_pcg_index - expected_next_index;
            }
        }
        expected_next_index = frame_header.first_pcg_index + PCG_FRAME_SAMPLES;

        for (size_t base = 0U; base < PCG_FRAME_SAMPLES; base += SD_CSV_CHUNK_SAMPLES) {
            size_t used = 0U;
            const size_t end = (base + SD_CSV_CHUNK_SAMPLES <= PCG_FRAME_SAMPLES)
                                   ? base + SD_CSV_CHUNK_SAMPLES
                                   : PCG_FRAME_SAMPLES;

            for (size_t i = base; i < end; ++i) {
                const uint64_t sample_index = frame_header.first_pcg_index + i;
                const uint64_t timestamp_us =
                    (sample_index * 1000000ULL) / PCG_SAMPLE_RATE;
                const bool is_ecg_anchor =
                    (sample_index % PCG_PER_ECG_SLOT) == 0U;

                while (has_ecg && next_ecg.pcg_anchor_index < sample_index) {
                    ++out_stats->ecg_records_unmapped;
                    if (!read_next_ecg(ecg_file, &ecg_remaining,
                                       &next_ecg, &has_ecg)) {
                        ESP_LOGE(TAG, "ECG raw read failed during conversion");
                        fclose(csv_file);
                        fclose(pcg_file);
                        fclose(ecg_file);
                        return ESP_FAIL;
                    }
                }

                int32_t ecg_value = 0;
                uint8_t flags = s_convert_pcg_valid[i] != 0U
                                    ? CSV_FLAG_PCG_VALID
                                    : 0U;

                if (has_ecg && next_ecg.pcg_anchor_index == sample_index) {
                    ecg_value = (int32_t)next_ecg.raw;
                    flags |= build_ecg_flags(&next_ecg);
                    ++out_stats->ecg_records_mapped;
                    if (!read_next_ecg(ecg_file, &ecg_remaining,
                                       &next_ecg, &has_ecg)) {
                        ESP_LOGE(TAG, "ECG raw read failed after mapping");
                        fclose(csv_file);
                        fclose(pcg_file);
                        fclose(ecg_file);
                        return ESP_FAIL;
                    }
                } else if (is_ecg_anchor) {
                    flags |= CSV_FLAG_ECG_MISSING;
                    ++out_stats->ecg_slots_missing_in_csv;
                }

                const int n = snprintf(&s_csv_text_buffer[used],
                                       sizeof(s_csv_text_buffer) - used,
                                       "%" PRIu64 ",%" PRIu64 ",%" PRId32
                                       ",%" PRId32 ",%u\n",
                                       sample_index,
                                       timestamp_us,
                                       ecg_value,
                                       s_convert_pcg_samples[i],
                                       (unsigned)flags);
                if (n < 0 || (size_t)n >= sizeof(s_csv_text_buffer) - used) {
                    ESP_LOGE(TAG, "CSV text buffer overflow at sample=%" PRIu64,
                             sample_index);
                    fclose(csv_file);
                    fclose(pcg_file);
                    fclose(ecg_file);
                    return ESP_FAIL;
                }
                used += (size_t)n;
                ++out_stats->csv_samples_written;
            }

            if (fwrite(s_csv_text_buffer, 1U, used, csv_file) != used) {
                ESP_LOGE(TAG, "CSV write failed | errno=%d (%s)",
                         errno, strerror(errno));
                fclose(csv_file);
                fclose(pcg_file);
                fclose(ecg_file);
                return ESP_FAIL;
            }
        }
        ++out_stats->pcg_frames_written;
    }

    while (has_ecg) {
        ++out_stats->ecg_records_unmapped;
        if (!read_next_ecg(ecg_file, &ecg_remaining, &next_ecg, &has_ecg)) {
            ESP_LOGE(TAG, "ECG raw tail read failed");
            fclose(csv_file);
            fclose(pcg_file);
            fclose(ecg_file);
            return ESP_FAIL;
        }
    }

    const int flush_rc = fflush(csv_file);
    const int close_rc = fclose(csv_file);
    fclose(pcg_file);
    fclose(ecg_file);
    if (flush_rc != 0 || close_rc != 0) {
        ESP_LOGE(TAG, "Cannot close CSV | errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "CSV conversion complete: frames=%" PRIu32
             " mapped=%" PRIu32 " missing=%" PRIu32
             " raw_gaps=%" PRIu32 " gap_samples=%" PRIu64,
             out_stats->pcg_frames_written,
             out_stats->ecg_records_mapped,
             out_stats->ecg_slots_missing_in_csv,
             out_stats->pcg_frame_gaps,
             out_stats->pcg_samples_missing_from_raw);
    return ESP_OK;
}

esp_err_t sd_writer_deinit(void)
{
    (void)sd_writer_close_raw_record(0);
    if (s_card != NULL) {
        const esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_card = NULL;
    }
    const esp_err_t bus_ret = spi_bus_free(SD_SPI_HOST);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_free: %s", esp_err_to_name(bus_ret));
        return bus_ret;
    }
    ESP_LOGI(TAG, "SD writer deinitialized");
    return ESP_OK;
}

const char *sd_writer_get_filename(void) { return s_csv_filename; }
const char *sd_writer_get_pcg_raw_filename(void) { return s_pcg_raw_filename; }
const char *sd_writer_get_ecg_raw_filename(void) { return s_ecg_raw_filename; }

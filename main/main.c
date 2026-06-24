#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "ecg_adc.h"
#include "file_crc32.h"
#include "i2s_mic.h"
#include "sd_writer.h"
#include "web_server.h"

static const char *TAG = "ECG_PCG";

#define EVT_RECORD_START        BIT0
#define EVT_STORAGE_READY       BIT1
#define EVT_PCG_STREAM_STARTED  BIT2
#define EVT_PCG_DONE            BIT3
#define EVT_ECG_DONE            BIT4
#define EVT_STORAGE_DONE        BIT5
#define EVT_ABORT               BIT6

static EventGroupHandle_t s_events = NULL;

/* Ownership:
 * PCG: free -> Core0 fill -> pcg_ready -> SD writer -> free
 * ECG: free -> Core1 fill -> ecg_ready -> SD writer -> free
 */
static QueueHandle_t s_pcg_free = NULL;
static QueueHandle_t s_pcg_ready = NULL;
static QueueHandle_t s_ecg_free = NULL;
static QueueHandle_t s_ecg_ready = NULL;

static pcg_frame_t *s_pcg_frame_pool = NULL;
static ecg_batch_t *s_ecg_batch_pool = NULL;
static int32_t s_pcg_discard_slot[PCG_PER_ECG_SLOT];

static TaskHandle_t s_ecg_task_handle = NULL;
static TaskHandle_t s_storage_task_handle = NULL;
static TaskHandle_t s_main_task_handle = NULL;

/* Static session object prevents large post-record data from using main stack. */
static web_session_info_t s_web_info;

typedef struct {
    sd_conversion_stats_t conversion;
    esp_err_t conversion_ret;
    esp_err_t crc_ret;
    uint32_t crc32;
    uint64_t csv_size_bytes;
    UBaseType_t stack_high_water;
} postprocess_result_t;
static postprocess_result_t s_postprocess;

/* One producer owns each counter during capture. Values are read only after
 * EVT_STORAGE_DONE, so no mutex is needed for the summary. */
static volatile uint32_t s_pcg_frames_captured = 0U;
static volatile uint32_t s_pcg_frames_dropped = 0U;
static volatile uint32_t s_pcg_short_frames = 0U;
static volatile uint32_t s_pcg_ready_queue_drops = 0U;
static volatile uint32_t s_ecg_samples_captured = 0U;
static volatile uint32_t s_ecg_samples_dropped = 0U;
static volatile uint32_t s_ecg_batches_dropped = 0U;
static volatile uint32_t s_ecg_late_notifications = 0U;
static volatile uint32_t s_ecg_max_pending = 0U;
static volatile uint32_t s_ecg_lead_off_samples = 0U;
static volatile uint32_t s_ecg_lo_plus_samples = 0U;
static volatile uint32_t s_ecg_lo_minus_samples = 0U;
static volatile uint32_t s_sd_write_errors = 0U;
static volatile int64_t s_pcg_start_time_us = 0;
static volatile int64_t s_ecg_first_offset_us = -1;

static void wait_for_start_barrier(void)
{
    (void)xEventGroupWaitBits(s_events, EVT_RECORD_START,
                              pdFALSE, pdTRUE, portMAX_DELAY);
}

static bool abort_requested(void)
{
    return (xEventGroupGetBits(s_events) & EVT_ABORT) != 0U;
}

static void wake_storage_task(void)
{
    if (s_storage_task_handle != NULL) {
        xTaskNotifyGive(s_storage_task_handle);
    }
}

static void log_task_stack(const char *name)
{
    const UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "%s stack high-water=%u", name, (unsigned)watermark);
}

/* ============================================================
 * CORE 0: PCG master timeline
 *
 * A token is issued immediately before I2S waits for a 40-sample slot.
 * Therefore ECG slot k is anchored to PCG index k*40.
 * ============================================================ */
static void i2s_reader_task(void *arg)
{
    (void)arg;
    wait_for_start_barrier();

    const EventBits_t storage_ready = xEventGroupWaitBits(
        s_events, EVT_STORAGE_READY | EVT_ABORT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if ((storage_ready & EVT_ABORT) != 0U) {
        xEventGroupSetBits(s_events, EVT_PCG_DONE);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Core0 PCG master running on core %d", xPortGetCoreID());
    if (i2s_mic_start() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start I2S stream");
        xEventGroupSetBits(s_events, EVT_ABORT | EVT_PCG_DONE);
        vTaskDelete(NULL);
        return;
    }

    s_pcg_start_time_us = esp_timer_get_time();
    xEventGroupSetBits(s_events, EVT_PCG_STREAM_STARTED);

    uint64_t slot_id = 0U;
    uint64_t pcg_index = 0U;

    for (uint64_t frame_id = 0U;
         frame_id < RECORD_TOTAL_FRAMES && !abort_requested();
         ++frame_id) {

        pcg_frame_t *frame = NULL;
        const bool have_frame =
            xQueueReceive(s_pcg_free, &frame,
                          pdMS_TO_TICKS(CORE0_FRAME_WAIT_MS)) == pdTRUE;

        if (have_frame) {
            memset(frame, 0, sizeof(*frame));
            frame->first_slot_id = slot_id;
            frame->first_pcg_index = pcg_index;
        } else {
            ++s_pcg_frames_dropped;
            ESP_LOGW(TAG,
                     "PCG frame pool exhausted: drop frame=%" PRIu64
                     " index=%" PRIu64 " free=%u ready=%u",
                     frame_id, pcg_index,
                     (unsigned)uxQueueMessagesWaiting(s_pcg_free),
                     (unsigned)uxQueueMessagesWaiting(s_pcg_ready));
        }

        bool frame_short = false;
        for (size_t local_slot = 0U;
             local_slot < SYNC_SLOTS_PER_FRAME;
             ++local_slot) {

            /* One logical ECG trigger per 40 PCG samples. This uses a task
             * notification counter, not a bounded token queue. */
            if (s_ecg_task_handle != NULL) {
                xTaskNotifyGive(s_ecg_task_handle);
            } else {
                xEventGroupSetBits(s_events, EVT_ABORT);
            }

            int32_t *destination = have_frame
                ? &frame->pcg_samples[local_slot * PCG_PER_ECG_SLOT]
                : s_pcg_discard_slot;

            size_t received = 0U;
            const esp_err_t read_ret = i2s_mic_read_samples(
                destination,
                PCG_PER_ECG_SLOT,
                &received,
                I2S_READ_TIMEOUT_MS);

            if (received > PCG_PER_ECG_SLOT) {
                received = PCG_PER_ECG_SLOT;
            }
            if (read_ret != ESP_OK || received != PCG_PER_ECG_SLOT) {
                frame_short = true;
            }

            if (have_frame) {
                const size_t offset = local_slot * PCG_PER_ECG_SLOT;
                for (size_t j = 0U; j < PCG_PER_ECG_SLOT; ++j) {
                    const bool valid = (read_ret == ESP_OK && j < received);
                    frame->pcg_valid[offset + j] = valid ? 1U : 0U;
                    if (valid) {
                        ++frame->valid_sample_count;
                    } else {
                        frame->pcg_samples[offset + j] = 0;
                    }
                }
            }

            ++slot_id;
            pcg_index += PCG_PER_ECG_SLOT;
        }

        if (!have_frame) {
            continue;
        }
        if (frame_short) {
            ++s_pcg_short_frames;
        }

        if (xQueueSend(s_pcg_ready, &frame,
                       pdMS_TO_TICKS(CORE0_FRAME_WAIT_MS)) == pdTRUE) {
            ++s_pcg_frames_captured;
            wake_storage_task();
        } else {
            ++s_pcg_frames_dropped;
            ++s_pcg_ready_queue_drops;
            ESP_LOGW(TAG, "PCG ready queue full: drop frame=%" PRIu64,
                     frame_id);
            (void)xQueueSend(s_pcg_free, &frame, portMAX_DELAY);
        }
    }

    (void)i2s_mic_stop();

    /* I2S has stopped, so waiting for end-marker space cannot lose samples. */
    pcg_frame_t *pcg_end = NULL;
    (void)xQueueSend(s_pcg_ready, &pcg_end, portMAX_DELAY);
    wake_storage_task();
    xEventGroupSetBits(s_events, EVT_PCG_DONE);

    ESP_LOGI(TAG,
             "Core0 PCG complete: slots=%" PRIu64 " PCG=%" PRIu64
             " frames=%" PRIu32 " drop=%" PRIu32
             " short=%" PRIu32 " queue_drop=%" PRIu32,
             slot_id, pcg_index,
             s_pcg_frames_captured, s_pcg_frames_dropped,
             s_pcg_short_frames, s_pcg_ready_queue_drops);
    log_task_stack("pcg_master");
    vTaskDelete(NULL);
}

/* ============================================================
 * CORE 1: ECG slave to PCG notifications
 * ============================================================ */
static void flush_ecg_batch(ecg_batch_t **batch_ptr, bool final_flush)
{
    if (batch_ptr == NULL || *batch_ptr == NULL) {
        return;
    }

    ecg_batch_t *batch = *batch_ptr;
    if (batch->count == 0U) {
        (void)xQueueSend(s_ecg_free, &batch, portMAX_DELAY);
        *batch_ptr = NULL;
        return;
    }

    const TickType_t wait = final_flush ? portMAX_DELAY
                                        : pdMS_TO_TICKS(ECG_BATCH_QUEUE_WAIT_MS);
    if (xQueueSend(s_ecg_ready, &batch, wait) == pdTRUE) {
        wake_storage_task();
    } else {
        s_ecg_samples_dropped += batch->count;
        ++s_ecg_batches_dropped;
        ESP_LOGW(TAG, "ECG batch queue full: drop %u samples",
                 (unsigned)batch->count);
        (void)xQueueSend(s_ecg_free, &batch, portMAX_DELAY);
    }
    *batch_ptr = NULL;
}

static void sample_ecg_slot(ecg_batch_t **batch_ptr, uint64_t slot_id)
{
    uint8_t lead_status = ECG_LEAD_STATUS_NONE;
    uint16_t raw = 0U;

    /* Always perform the physical ADC conversion even if a storage batch is
     * temporarily unavailable. This keeps the actual ECG acquisition cadence
     * at 400 Hz; only persistence would be flagged as a drop. */
    const esp_err_t lead_ret = ecg_adc_read_lead_status(&lead_status);
    const esp_err_t adc_ret = ecg_adc_read_raw(&raw);
    const int64_t capture_time_us = esp_timer_get_time();

    if (slot_id == 0U) {
        s_ecg_first_offset_us = capture_time_us - s_pcg_start_time_us;
    }

    if (lead_ret != ESP_OK || adc_ret != ESP_OK) {
        ++s_ecg_samples_dropped;
        return;
    }

    if ((lead_status & ECG_LEAD_STATUS_ANY) != 0U) {
        ++s_ecg_lead_off_samples;
    }
    if ((lead_status & ECG_LEAD_STATUS_LO_PLUS) != 0U) {
        ++s_ecg_lo_plus_samples;
    }
    if ((lead_status & ECG_LEAD_STATUS_LO_MINUS) != 0U) {
        ++s_ecg_lo_minus_samples;
    }

    if (*batch_ptr == NULL) {
        (void)xQueueReceive(s_ecg_free, batch_ptr, 0U);
        if (*batch_ptr != NULL) {
            memset(*batch_ptr, 0, sizeof(**batch_ptr));
        }
    }

    if (*batch_ptr == NULL) {
        ++s_ecg_samples_dropped;
        return;
    }

    ecg_batch_t *batch = *batch_ptr;
    ecg_sample_t *sample = &batch->samples[batch->count++];
    sample->slot_id = slot_id;
    sample->pcg_anchor_index = slot_id * PCG_PER_ECG_SLOT;
    sample->capture_time_us = capture_time_us;
    sample->raw = raw;
    sample->lead_status = lead_status;
    sample->valid = 1U;

    ++s_ecg_samples_captured;
    if (batch->count == SYNC_SLOTS_PER_FRAME) {
        flush_ecg_batch(batch_ptr, false);
    }
}

static void ecg_acquisition_task(void *arg)
{
    (void)arg;
    wait_for_start_barrier();

    const EventBits_t started = xEventGroupWaitBits(
        s_events, EVT_PCG_STREAM_STARTED | EVT_ABORT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if ((started & EVT_ABORT) != 0U) {
        xEventGroupSetBits(s_events, EVT_ECG_DONE);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Core1 ECG token-driven task running on core %d",
             xPortGetCoreID());

    ecg_batch_t *batch = NULL;
    uint64_t slot_id = 0U;

    while (slot_id < RECORD_TOTAL_SLOTS && !abort_requested()) {
        const uint32_t pending = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250U));
        if (pending == 0U) {
            continue;
        }
        if (pending > s_ecg_max_pending) {
            s_ecg_max_pending = pending;
        }
        if (pending > 1U) {
            s_ecg_late_notifications += pending - 1U;
        }

        uint32_t remaining = pending;
        while (remaining-- > 0U && slot_id < RECORD_TOTAL_SLOTS) {
            sample_ecg_slot(&batch, slot_id);
            ++slot_id;
        }
    }

    flush_ecg_batch(&batch, true);

    ecg_batch_t *ecg_end = NULL;
    (void)xQueueSend(s_ecg_ready, &ecg_end, portMAX_DELAY);
    wake_storage_task();
    xEventGroupSetBits(s_events, EVT_ECG_DONE);

    ESP_LOGI(TAG,
             "Core1 ECG complete: slots=%" PRIu64
             " valid=%" PRIu32 " drop=%" PRIu32
             " late=%" PRIu32 " max_pending=%" PRIu32
             " first_offset=%" PRId64 " us",
             slot_id, s_ecg_samples_captured, s_ecg_samples_dropped,
             s_ecg_late_notifications, s_ecg_max_pending,
             s_ecg_first_offset_us);
    log_task_stack("ecg_token");
    vTaskDelete(NULL);
}

/* ============================================================
 * CORE 1: binary SD writer
 *
 * PCG is always dequeued first. Returning a PCG frame quickly is more
 * important than writing a small ECG batch because Core 0 cannot pause I2S.
 * ============================================================ */
static bool storage_process_one_pcg(bool *pcg_finished)
{
    if (*pcg_finished) {
        return false;
    }

    pcg_frame_t *frame = NULL;
    if (xQueueReceive(s_pcg_ready, &frame, 0U) != pdTRUE) {
        return false;
    }
    if (frame == NULL) {
        *pcg_finished = true;
        return true;
    }

    if (sd_writer_write_pcg_frame(frame) != ESP_OK) {
        ++s_sd_write_errors;
        ESP_LOGE(TAG, "PCG binary write failed at index=%" PRIu64,
                 frame->first_pcg_index);
    }
    (void)xQueueSend(s_pcg_free, &frame, portMAX_DELAY);
    return true;
}

static bool storage_process_one_ecg(bool *ecg_finished)
{
    if (*ecg_finished) {
        return false;
    }

    ecg_batch_t *batch = NULL;
    if (xQueueReceive(s_ecg_ready, &batch, 0U) != pdTRUE) {
        return false;
    }
    if (batch == NULL) {
        *ecg_finished = true;
        return true;
    }

    if (sd_writer_write_ecg_batch(batch) != ESP_OK) {
        ++s_sd_write_errors;
        ESP_LOGE(TAG, "ECG binary write failed");
    }
    (void)xQueueSend(s_ecg_free, &batch, portMAX_DELAY);
    return true;
}

static void storage_task(void *arg)
{
    (void)arg;
    wait_for_start_barrier();

    /* This can take time because FAT clusters are reserved here, before I2S. */
    if (sd_writer_begin_raw_record(RECORD_TOTAL_SLOTS) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot prepare binary storage");
        xEventGroupSetBits(s_events, EVT_ABORT | EVT_STORAGE_DONE);
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(s_events, EVT_STORAGE_READY);

    const EventBits_t started = xEventGroupWaitBits(
        s_events, EVT_PCG_STREAM_STARTED | EVT_ABORT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if ((started & EVT_ABORT) != 0U) {
        (void)sd_writer_close_raw_record(0);
        xEventGroupSetBits(s_events, EVT_STORAGE_DONE);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Core1 binary SD writer running on core %d",
             xPortGetCoreID());

    bool pcg_finished = false;
    bool ecg_finished = false;
    while (!pcg_finished || !ecg_finished) {
        bool processed = false;

        /* Priority order is deliberate: release PCG frames first. */
        processed |= storage_process_one_pcg(&pcg_finished);
        processed |= storage_process_one_ecg(&ecg_finished);

        if (!processed) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
        }
    }

    if (sd_writer_close_raw_record(s_pcg_start_time_us) != ESP_OK) {
        ++s_sd_write_errors;
    }
    xEventGroupSetBits(s_events, EVT_STORAGE_DONE);

    ESP_LOGI(TAG, "Core1 binary SD writer drained");
    log_task_stack("binary_sd");
    vTaskDelete(NULL);
}

/* ============================================================
 * POST-RECORD conversion
 * ============================================================ */
static void postprocess_task(void *arg)
{
    (void)arg;
    memset(&s_postprocess, 0, sizeof(s_postprocess));

    s_postprocess.conversion_ret = sd_writer_convert_raw_to_csv(
        &s_postprocess.conversion);
    s_postprocess.crc_ret = (s_postprocess.conversion_ret == ESP_OK)
        ? file_crc32_compute(sd_writer_get_filename(),
                             &s_postprocess.crc32,
                             &s_postprocess.csv_size_bytes)
        : ESP_FAIL;

    s_postprocess.stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "postprocess stack high-water=%u",
             (unsigned)s_postprocess.stack_high_water);

    xTaskNotifyGive(s_main_task_handle);
    vTaskDelete(NULL);
}

static void destroy_capture_objects(void)
{
    if (s_pcg_free != NULL) { vQueueDelete(s_pcg_free); s_pcg_free = NULL; }
    if (s_pcg_ready != NULL) { vQueueDelete(s_pcg_ready); s_pcg_ready = NULL; }
    if (s_ecg_free != NULL) { vQueueDelete(s_ecg_free); s_ecg_free = NULL; }
    if (s_ecg_ready != NULL) { vQueueDelete(s_ecg_ready); s_ecg_ready = NULL; }
    if (s_events != NULL) { vEventGroupDelete(s_events); s_events = NULL; }

    free(s_pcg_frame_pool);
    free(s_ecg_batch_pool);
    s_pcg_frame_pool = NULL;
    s_ecg_batch_pool = NULL;
    s_ecg_task_handle = NULL;
    s_storage_task_handle = NULL;
}

static bool create_capture_objects(void)
{
    ESP_LOGI(TAG, "Heap before capture pool: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    s_pcg_frame_pool = heap_caps_calloc(PCG_FRAME_POOL_SIZE,
                                        sizeof(*s_pcg_frame_pool),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_ecg_batch_pool = heap_caps_calloc(ECG_BATCH_POOL_SIZE,
                                        sizeof(*s_ecg_batch_pool),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_events = xEventGroupCreate();
    s_pcg_free = xQueueCreate(PCG_FRAME_POOL_SIZE, sizeof(pcg_frame_t *));
    s_pcg_ready = xQueueCreate(PCG_READY_QUEUE_LENGTH, sizeof(pcg_frame_t *));
    s_ecg_free = xQueueCreate(ECG_BATCH_POOL_SIZE, sizeof(ecg_batch_t *));
    s_ecg_ready = xQueueCreate(ECG_READY_QUEUE_LENGTH, sizeof(ecg_batch_t *));

    if (s_pcg_frame_pool == NULL || s_ecg_batch_pool == NULL ||
        s_events == NULL || s_pcg_free == NULL || s_pcg_ready == NULL ||
        s_ecg_free == NULL || s_ecg_ready == NULL) {
        ESP_LOGE(TAG, "Cannot allocate capture pools/queues");
        destroy_capture_objects();
        return false;
    }

    for (size_t i = 0U; i < PCG_FRAME_POOL_SIZE; ++i) {
        pcg_frame_t *frame = &s_pcg_frame_pool[i];
        (void)xQueueSend(s_pcg_free, &frame, portMAX_DELAY);
    }
    for (size_t i = 0U; i < ECG_BATCH_POOL_SIZE; ++i) {
        ecg_batch_t *batch = &s_ecg_batch_pool[i];
        (void)xQueueSend(s_ecg_free, &batch, portMAX_DELAY);
    }

    ESP_LOGI(TAG,
             "Pool initialized: PCG frames=%u (%u ms), ECG batches=%u (%u slots)",
             PCG_FRAME_POOL_SIZE,
             (unsigned)(PCG_FRAME_POOL_SIZE * PCG_FRAME_DURATION_US / 1000U),
             ECG_BATCH_POOL_SIZE,
             ECG_BATCH_POOL_SIZE * SYNC_SLOTS_PER_FRAME);
    return true;
}

static bool capture_is_lossless(void)
{
    return s_pcg_frames_captured == RECORD_TOTAL_FRAMES &&
           s_pcg_frames_dropped == 0U &&
           s_pcg_short_frames == 0U &&
           s_pcg_ready_queue_drops == 0U &&
           s_ecg_samples_captured == RECORD_TOTAL_SLOTS &&
           s_ecg_samples_dropped == 0U &&
           s_ecg_late_notifications == 0U &&
           s_sd_write_errors == 0U;
}

void app_main(void)
{
    s_main_task_handle = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG, "===== ECG + PCG: PCG-master 40:1 binary capture =====");
    ESP_LOGI(TAG,
             "PCG=%u Hz | ECG=%u Hz | slot=%u PCG | frame=%u PCG / %u ECG | record=%u s",
             PCG_SAMPLE_RATE, ECG_SAMPLE_RATE,
             PCG_PER_ECG_SLOT, PCG_FRAME_SAMPLES,
             SYNC_SLOTS_PER_FRAME, RECORD_DURATION_SECONDS);

    if (i2s_mic_init() != ESP_OK ||
        ecg_adc_init() != ESP_OK ||
        sd_writer_init() != ESP_OK) {
        (void)sd_writer_deinit();
        (void)ecg_adc_deinit();
        (void)i2s_mic_deinit();
        return;
    }

    if (!create_capture_objects()) {
        (void)sd_writer_deinit();
        (void)ecg_adc_deinit();
        (void)i2s_mic_deinit();
        return;
    }

    TaskHandle_t pcg_task = NULL;
    TaskHandle_t sd_task = NULL;
    const BaseType_t pcg_ok = xTaskCreatePinnedToCore(
        i2s_reader_task, "pcg_master", I2S_TASK_STACK_SIZE,
        NULL, I2S_TASK_PRIORITY, &pcg_task, I2S_TASK_CORE);
    const BaseType_t ecg_ok = xTaskCreatePinnedToCore(
        ecg_acquisition_task, "ecg_token", ECG_TASK_STACK_SIZE,
        NULL, ECG_TASK_PRIORITY, &s_ecg_task_handle, ECG_TASK_CORE);
    const BaseType_t sd_ok = xTaskCreatePinnedToCore(
        storage_task, "binary_sd", SD_TASK_STACK_SIZE,
        NULL, SD_TASK_PRIORITY, &sd_task, SD_TASK_CORE);
    s_storage_task_handle = sd_task;

    if (pcg_ok != pdPASS || ecg_ok != pdPASS || sd_ok != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        if (pcg_task != NULL) vTaskDelete(pcg_task);
        if (s_ecg_task_handle != NULL) vTaskDelete(s_ecg_task_handle);
        if (sd_task != NULL) vTaskDelete(sd_task);
        destroy_capture_objects();
        (void)sd_writer_deinit();
        (void)ecg_adc_deinit();
        (void)i2s_mic_deinit();
        return;
    }

    ESP_LOGI(TAG,
             "Start barrier released: slots=%" PRIu64
             " PCG=%" PRIu64 " frames=%" PRIu64,
             RECORD_TOTAL_SLOTS, RECORD_TOTAL_PCG_SAMPLES,
             RECORD_TOTAL_FRAMES);
    xEventGroupSetBits(s_events, EVT_RECORD_START);

    (void)xEventGroupWaitBits(s_events, EVT_STORAGE_DONE,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    const bool lossless = capture_is_lossless();
    ESP_LOGI(TAG,
             "Capture check: %s | PCG frames=%" PRIu32 "/%" PRIu64
             " drop=%" PRIu32 " short=%" PRIu32
             " | ECG=%" PRIu32 "/%" PRIu64
             " drop=%" PRIu32 " late=%" PRIu32
             " | SD errors=%" PRIu32,
             lossless ? "PASS" : "FAIL",
             s_pcg_frames_captured, RECORD_TOTAL_FRAMES,
             s_pcg_frames_dropped, s_pcg_short_frames,
             s_ecg_samples_captured, RECORD_TOTAL_SLOTS,
             s_ecg_samples_dropped, s_ecg_late_notifications,
             s_sd_write_errors);

    (void)ecg_adc_deinit();
    (void)i2s_mic_deinit();
    destroy_capture_objects();

    TaskHandle_t post_task = NULL;
    if (xTaskCreatePinnedToCore(postprocess_task, "postprocess",
                                POSTPROCESS_TASK_STACK_SIZE, NULL,
                                POSTPROCESS_TASK_PRIORITY, &post_task,
                                POSTPROCESS_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create postprocess task; raw files stay on SD");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG,
             "Postprocess: convert=%s crc=%s CSV=%" PRIu64 " bytes",
             esp_err_to_name(s_postprocess.conversion_ret),
             esp_err_to_name(s_postprocess.crc_ret),
             s_postprocess.csv_size_bytes);

    if (s_postprocess.conversion_ret != ESP_OK ||
        s_postprocess.crc_ret != ESP_OK) {
        ESP_LOGE(TAG, "CSV/CRC failed. Raw files are retained for diagnosis.");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    memset(&s_web_info, 0, sizeof(s_web_info));
    snprintf(s_web_info.filename, sizeof(s_web_info.filename), "%s",
             sd_writer_get_filename());
    snprintf(s_web_info.pcg_raw_filename, sizeof(s_web_info.pcg_raw_filename), "%s",
             sd_writer_get_pcg_raw_filename());
    snprintf(s_web_info.ecg_raw_filename, sizeof(s_web_info.ecg_raw_filename), "%s",
             sd_writer_get_ecg_raw_filename());
    s_web_info.total_pcg_samples = RECORD_TOTAL_PCG_SAMPLES;
    s_web_info.pcg_blocks_captured = s_pcg_frames_captured;
    s_web_info.pcg_blocks_dropped = s_pcg_frames_dropped;
    s_web_info.pcg_short_blocks = s_pcg_short_frames;
    s_web_info.ecg_samples_captured = s_ecg_samples_captured;
    s_web_info.ecg_samples_dropped = s_ecg_samples_dropped;
    s_web_info.ecg_samples_missing_in_merge =
        s_postprocess.conversion.ecg_slots_missing_in_csv +
        s_postprocess.conversion.ecg_records_unmapped;
    s_web_info.sync_tokens_dropped = 0U;
    s_web_info.ecg_late_notifications = s_ecg_late_notifications;
    s_web_info.pcg_raw_frame_gaps = s_postprocess.conversion.pcg_frame_gaps;
    s_web_info.pcg_raw_gap_samples = s_postprocess.conversion.pcg_samples_missing_from_raw;
    s_web_info.ecg_lead_off_samples = s_ecg_lead_off_samples;
    s_web_info.ecg_lo_plus_samples = s_ecg_lo_plus_samples;
    s_web_info.ecg_lo_minus_samples = s_ecg_lo_minus_samples;
    s_web_info.sd_write_errors = s_sd_write_errors;
    s_web_info.ecg_first_offset_us = s_ecg_first_offset_us;
    s_web_info.file_crc32 = s_postprocess.crc32;
    s_web_info.file_size_bytes = s_postprocess.csv_size_bytes;
    s_web_info.crc32_valid = true;
    s_web_info.capture_valid = lossless &&
                               s_postprocess.conversion.pcg_frame_gaps == 0U &&
                               s_postprocess.conversion.ecg_slots_missing_in_csv == 0U;

    if (web_server_start(&s_web_info) != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed. CSV remains on SD: %s",
                 sd_writer_get_filename());
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    ESP_LOGI(TAG,
             "Session %s. Connect to Wi-Fi '%s', then browse http://192.168.4.1",
             lossless ? "VALID" : "HAS DATA-LOSS FLAGS",
             WEB_AP_SSID);
    log_task_stack("main");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"

/* ============================================================
 * CLOCK PLAN AND SYNCHRONIZATION GRID
 *
 * PCG is the master timeline.
 * 16,000 / 400 = 40, therefore every ECG slot owns exactly 40 PCG samples.
 * Core 0 issues one direct notification per PCG slot; Core 1 acquires exactly
 * one ECG sample per notification.
 * ============================================================ */
#define PCG_SAMPLE_RATE                 16000U
#define ECG_SAMPLE_RATE                 400U
#define PCG_PER_ECG_SLOT                (PCG_SAMPLE_RATE / ECG_SAMPLE_RATE) /* 40 */
#define ECG_SAMPLE_PERIOD_US            (1000000U / ECG_SAMPLE_RATE)         /* 2500 us */

/* One raw-storage frame = 20 slots = 800 PCG samples = 50 ms. */
#define SYNC_SLOTS_PER_FRAME            20U
#define PCG_FRAME_SAMPLES               (PCG_PER_ECG_SLOT * SYNC_SLOTS_PER_FRAME) /* 800 */
#define PCG_FRAME_DURATION_US           ((PCG_FRAME_SAMPLES * 1000000ULL) / PCG_SAMPLE_RATE)

/* Validation sequence: 2 s -> 10 s -> 60 s only after all loss counters are zero. */
#define RECORD_DURATION_SECONDS         2U
#define RECORD_TOTAL_SLOTS              ((uint64_t)RECORD_DURATION_SECONDS * ECG_SAMPLE_RATE)
#define RECORD_TOTAL_PCG_SAMPLES        (RECORD_TOTAL_SLOTS * PCG_PER_ECG_SLOT)
#define RECORD_TOTAL_FRAMES             (RECORD_TOTAL_SLOTS / SYNC_SLOTS_PER_FRAME)

/* ============================================================
 * PCG - ICS-43434
 * ============================================================ */
#define PCG_BITS_PER_SAMPLE             24U
#define PCG_DMA_BUF_COUNT               24U  /* 24 x 40 = 60 ms I2S DMA backlog */
#define PCG_DMA_BUF_LEN                 PCG_PER_ECG_SLOT

#define I2S_MIC_PORT                    I2S_NUM_0
#define I2S_BCLK_PIN                    GPIO_NUM_26
#define I2S_WS_PIN                      GPIO_NUM_25
#define I2S_DATA_PIN                    GPIO_NUM_34
#define I2S_READ_TIMEOUT_MS             200U

/* ============================================================
 * ECG - AD8232
 * ============================================================ */
#define ECG_ADC_UNIT                    ADC_UNIT_1
#define ECG_ADC_CHANNEL                 ADC_CHANNEL_0
#define ECG_ADC_GPIO                    GPIO_NUM_36
#define ECG_ADC_ATTEN                   ADC_ATTEN_DB_12
#define ECG_ADC_BITWIDTH                ADC_BITWIDTH_12

#define ECG_LO_PLUS_GPIO                GPIO_NUM_16
#define ECG_LO_MINUS_GPIO               GPIO_NUM_4

#define ECG_LEAD_STATUS_NONE            0U
#define ECG_LEAD_STATUS_LO_PLUS         (1U << 0)
#define ECG_LEAD_STATUS_LO_MINUS        (1U << 1)
#define ECG_LEAD_STATUS_ANY             (ECG_LEAD_STATUS_LO_PLUS | ECG_LEAD_STATUS_LO_MINUS)

/* ============================================================
 * SD card - SPI mode. This is the map that has mounted card SS04G.
 * ============================================================ */
#define SD_SPI_HOST                     SPI2_HOST
#define SD_MOSI_PIN                     GPIO_NUM_13
#define SD_MISO_PIN                     GPIO_NUM_12
#define SD_CLK_PIN                      GPIO_NUM_14
#define SD_CS_PIN                       GPIO_NUM_15
#define SD_MOUNT_POINT                  "/sdcard"

/* Short 8.3 filenames: Pxxx.BIN, Exxx.BIN, Cxxx.CSV. */
#define SD_PCG_RAW_PREFIX               "/sdcard/P"
#define SD_ECG_RAW_PREFIX               "/sdcard/E"
#define SD_CSV_PREFIX                   "/sdcard/C"

#define SD_SPI_FREQ_KHZ                 20000U
#define SD_MAX_TRANSFER_BYTES           (16U * 1024U)

/* Raw capture uses binary files and no explicit fflush while I2S is active.
 * Files are preallocated before the record barrier, so FAT cluster allocation
 * cannot stall the real-time phase. */
#define SD_PCG_IO_BUFFER_BYTES          (16U * 1024U)
#define SD_ECG_IO_BUFFER_BYTES          (4U * 1024U)
#define SD_CSV_IO_BUFFER_BYTES          (8U * 1024U)
#define SD_CSV_TEXT_BUFFER_BYTES        (16U * 1024U)
#define SD_CSV_CHUNK_SAMPLES            256U
#define SD_PREALLOCATE_RAW_FILES        1U
#define SD_FLUSH_EVERY_FRAMES           0U
#define SD_FLUSH_EVERY_ECG_BATCHES      0U

/* ============================================================
 * Capture pool and queue sizing
 *
 * PCG pool: 16 x 50 ms = 800 ms elasticity for SD write latency.
 * ECG pool: 24 batches x 20 ECG = 1.2 s elasticity.
 * ============================================================ */
#define PCG_FRAME_POOL_SIZE             16U
#define ECG_BATCH_POOL_SIZE             24U
#define PCG_READY_QUEUE_LENGTH          PCG_FRAME_POOL_SIZE
#define ECG_READY_QUEUE_LENGTH          ECG_BATCH_POOL_SIZE
#define CORE0_FRAME_WAIT_MS             20U
#define ECG_BATCH_QUEUE_WAIT_MS         0U

/* ============================================================
 * Task allocation
 * ============================================================ */
#define I2S_TASK_CORE                   0
#define I2S_TASK_PRIORITY               12
#define I2S_TASK_STACK_SIZE             4096

#define ECG_TASK_CORE                   1
#define ECG_TASK_PRIORITY               11
#define ECG_TASK_STACK_SIZE             4096

#define SD_TASK_CORE                    1
#define SD_TASK_PRIORITY                8
#define SD_TASK_STACK_SIZE              6144

/* Conversion runs only after I2S and capture tasks have stopped. It has an
 * independent stack so app_main can never overflow during BIN -> CSV. */
#define POSTPROCESS_TASK_CORE           1
#define POSTPROCESS_TASK_PRIORITY       6
#define POSTPROCESS_TASK_STACK_SIZE     12288

/* ============================================================
 * Local web display
 * ============================================================ */
#define WEB_AP_SSID                     "ECG-PCG-ESP32"
#define WEB_AP_PASSWORD                 "12345678"
#define WEB_AP_CHANNEL                  1U
#define WEB_AP_MAX_CLIENTS              4U
#define WEB_PREVIEW_DEFAULT_POINTS      1000U
#define WEB_PREVIEW_MAX_POINTS          1600U

/* ============================================================
 * CSV flags
 * ============================================================ */
#define CSV_FLAG_PCG_VALID              (1U << 0)
#define CSV_FLAG_ECG_VALID              (1U << 1)
#define CSV_FLAG_ECG_LEAD_OFF           (1U << 2)
#define CSV_FLAG_ECG_LO_PLUS            (1U << 3)
#define CSV_FLAG_ECG_LO_MINUS           (1U << 4)
#define CSV_FLAG_ECG_MISSING            (1U << 5)

/* ============================================================
 * In-memory records
 * ============================================================ */
typedef struct {
    uint64_t slot_id;
    uint64_t pcg_anchor_index;  /* invariant: slot_id * 40 */
    int64_t  capture_time_us;   /* captures software scheduling jitter */
    uint16_t raw;
    uint8_t  lead_status;
    uint8_t  valid;
} ecg_sample_t;

typedef struct {
    uint16_t count;
    uint16_t reserved;
    ecg_sample_t samples[SYNC_SLOTS_PER_FRAME];
} ecg_batch_t;

typedef struct {
    uint64_t first_slot_id;
    uint64_t first_pcg_index;
    uint16_t valid_sample_count;
    uint16_t reserved;
    int32_t  pcg_samples[PCG_FRAME_SAMPLES];
    uint8_t  pcg_valid[PCG_FRAME_SAMPLES];
} pcg_frame_t;

_Static_assert((PCG_SAMPLE_RATE % ECG_SAMPLE_RATE) == 0U,
               "PCG/ECG ratio must be an integer");
_Static_assert((PCG_FRAME_SAMPLES % PCG_PER_ECG_SLOT) == 0U,
               "PCG frame must contain whole synchronization slots");
_Static_assert((RECORD_TOTAL_SLOTS % SYNC_SLOTS_PER_FRAME) == 0U,
               "Record duration must contain an integer number of frames");

#endif /* APP_CONFIG_H */

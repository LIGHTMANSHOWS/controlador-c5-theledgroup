#include "sd_storage.h"

#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol.h"
#include "runtime_config.h"
#include "leds.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#ifndef CONFIG_LED_FLAG_SD_DEFAULT_FILE
#define CONFIG_LED_FLAG_SD_DEFAULT_FILE "/sdcard/test.lfs"
#endif
#ifndef CONFIG_LED_FLAG_SD_RECORD_FPS
#define CONFIG_LED_FLAG_SD_RECORD_FPS 30
#endif
#ifndef CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN
#define CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN 4
#endif

static bool s_mounted;
static char s_last_error[64] = "not initialized";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_playback_control_lock;
static QueueHandle_t s_record_queue;
static TaskHandle_t s_record_task_handle;
static TaskHandle_t s_playback_task_handle;
static uint8_t *s_record_buffers[CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN];
static bool s_record_buffer_busy[CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN];
static FILE *s_record_file;
static char s_record_path[96];
static uint32_t s_recorded_frames;
static uint32_t s_record_dropped_frames;
static uint32_t s_record_max_frames;
static uint32_t s_record_checksum;
static uint32_t s_last_playback_checksum;
static uint16_t s_record_fps;
static bool s_recording;
static bool s_record_stopping;
static bool s_record_show_leds = true;
static bool s_record_have_frame_id;
static uint16_t s_record_last_frame_id;
static uint32_t s_record_sequence_gaps;
static bool s_playing;
static bool s_playback_loop;
static uint16_t s_playback_fps;
static bool s_playback_clock_sync = true;
static uint16_t s_playback_file_fps;
static uint32_t s_playback_frames;
static uint32_t s_playback_loops;
static uint32_t s_playback_starts;
static uint32_t s_playback_duplicate_starts;
static int s_next_rec_index;
static FILE *s_upload_file;
static char s_upload_path[96];
static uint32_t s_upload_expected_size;
static uint32_t s_upload_expected_checksum;
static uint32_t s_upload_received;
static uint32_t s_upload_checksum;
static uint8_t *s_mem_record;
static uint32_t s_mem_capacity_frames;
static uint32_t s_mem_recorded_frames;
static bool s_mem_mode;
static bool s_playback_memory;
static uint8_t *s_rle_last_frame;
static uint16_t s_rle_repeat;
static bool s_rle_have_pending;
static uint32_t s_recorded_runs;

#define MEMORY_RECORD_PATH "mem://record"
#define MEMORY_RECORD_DEFAULT_FRAMES 180U

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t pixel_count;
    uint8_t output_count;
    uint8_t pixel_format;
    uint16_t pixels_per_output;
    uint16_t fps;
    uint32_t frame_size;
    uint32_t frame_count;
    uint32_t flags;
} sd_show_header_t;

typedef struct __attribute__((packed)) {
    uint16_t repeat;
    uint16_t payload_size;
} sd_rle_record_header_t;

typedef struct {
    uint8_t *data;
    uint16_t frame_id;
    int buffer_index;
} record_item_t;

#define SD_SHOW_MAGIC "LFS2"
#define SD_SHOW_VERSION 2
#define SD_SHOW_FLAG_RLE_REPEAT_PREVIOUS 0x00000001U

static bool lfs_file_is_valid(const char *path);

#define PLAYBACK_STOP_TIMEOUT_MS 2000

static esp_err_t stop_playback_task_and_wait(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_playing = false;
    bool stopped = s_playback_task_handle == NULL;
    xSemaphoreGive(s_lock);

    int waited_ms = 0;
    while (!stopped && waited_ms < PLAYBACK_STOP_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stopped = s_playback_task_handle == NULL;
        xSemaphoreGive(s_lock);
    }
    if (!stopped) {
        ESP_LOGE(TAG, "Timeout esperando detener playback SD");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static uint32_t checksum32_update(uint32_t checksum, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= data[i];
        checksum += 0x9e3779b9U;
    }
    return checksum;
}

static bool write_initial_header(FILE *file, uint16_t fps, uint32_t frame_count)
{
    sd_show_header_t header = {
        .magic = {'L', 'F', 'S', '2'},
        .version = SD_SHOW_VERSION,
        .pixel_count = LED_FLAG_TOTAL_LEDS,
        .output_count = LED_FLAG_OUTPUT_COUNT,
        .pixel_format = LED_FLAG_PIXEL_FORMAT_RGB555_BE,
        .pixels_per_output = LED_FLAG_LEDS_PER_OUTPUT,
        .fps = fps,
        .frame_size = LED_FLAG_FRAME_SIZE_BYTES,
        .frame_count = frame_count,
        .flags = SD_SHOW_FLAG_RLE_REPEAT_PREVIOUS,
    };
    size_t written = fwrite(&header, 1, sizeof(header), file);
    int flush_ret = fflush(file);
    int fd = fileno(file);
    int sync_ret = fd >= 0 ? fsync(fd) : -1;
    return written == sizeof(header) && flush_ret == 0 && sync_ret == 0;
}

static bool write_record_header(FILE *file)
{
    if (file == NULL) {
        return false;
    }
    long current = ftell(file);
    if (current < 0) {
        current = 0;
    }
    sd_show_header_t header = {
        .magic = {'L', 'F', 'S', '2'},
        .version = SD_SHOW_VERSION,
        .pixel_count = LED_FLAG_TOTAL_LEDS,
        .output_count = LED_FLAG_OUTPUT_COUNT,
        .pixel_format = LED_FLAG_PIXEL_FORMAT_RGB555_BE,
        .pixels_per_output = LED_FLAG_LEDS_PER_OUTPUT,
        .fps = s_record_fps > 0 ? s_record_fps : CONFIG_LED_FLAG_SD_RECORD_FPS,
        .frame_size = LED_FLAG_FRAME_SIZE_BYTES,
        .frame_count = s_recorded_frames,
        .flags = SD_SHOW_FLAG_RLE_REPEAT_PREVIOUS,
    };
    clearerr(file);
    if (fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "No se pudo posicionar cabecera de %s errno=%d", s_record_path, errno);
        return false;
    }
    size_t written = fwrite(&header, 1, sizeof(header), file);
    int flush_ret = fflush(file);
    int fd = fileno(file);
    int sync_ret = fd >= 0 ? fsync(fd) : -1;
    bool ok = written == sizeof(header) && flush_ret == 0 && sync_ret == 0;
    if (!ok) {
        ESP_LOGE(TAG,
                 "Cabecera incompleta %s written=%u/%u flush=%d sync=%d errno=%d",
                 s_record_path,
                 (unsigned)written,
                 (unsigned)sizeof(header),
                 flush_ret,
                 sync_ret,
                 errno);
    }
    fseek(file, current, SEEK_SET);
    return ok;
}

static bool update_record_header(void)
{
    return write_record_header(s_record_file);
}

static bool flush_rle_run_locked(void)
{
    if (s_record_file == NULL || !s_rle_have_pending || s_rle_repeat == 0) {
        return true;
    }
    sd_rle_record_header_t run = {
        .repeat = s_rle_repeat,
        .payload_size = LED_FLAG_FRAME_SIZE_BYTES,
    };
    size_t header_written = fwrite(&run, 1, sizeof(run), s_record_file);
    size_t frame_written = fwrite(s_rle_last_frame, 1, LED_FLAG_FRAME_SIZE_BYTES, s_record_file);
    if (header_written != sizeof(run) || frame_written != LED_FLAG_FRAME_SIZE_BYTES) {
        ESP_LOGE(TAG, "Escritura RLE corta run=%u header=%u frame=%u",
                 s_rle_repeat,
                 (unsigned)header_written,
                 (unsigned)frame_written);
        return false;
    }
    s_recorded_runs++;
    s_rle_have_pending = false;
    s_rle_repeat = 0;
    return true;
}

static bool repair_closed_record_header(void)
{
    if (s_record_path[0] == '\0' || s_recorded_frames == 0) {
        return false;
    }
    FILE *file = fopen(s_record_path, "r+b");
    if (file == NULL) {
        ESP_LOGE(TAG, "No se pudo reabrir %s para cabecera errno=%d", s_record_path, errno);
        return false;
    }
    bool ok = write_record_header(file);
    fclose(file);
    if (ok) {
        ESP_LOGI(TAG,
                 "Cabecera LFS confirmada tras cierre: %s frames=%" PRIu32,
                 s_record_path,
                 s_recorded_frames);
    }
    return ok;
}

static void record_task(void *arg)
{
    (void)arg;
    record_item_t item;

    while (true) {
        if (xQueueReceive(s_record_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool can_write = s_recording &&
                         (s_record_max_frames == 0 || s_recorded_frames < s_record_max_frames) &&
                         ((s_record_file != NULL) ||
                          (s_mem_mode && s_mem_record != NULL && s_recorded_frames < s_mem_capacity_frames));
        bool memory_mode = s_mem_mode;
        xSemaphoreGive(s_lock);

        if (can_write) {
            size_t written = 0;
            if (memory_mode) {
                memcpy(s_mem_record + (s_recorded_frames * LED_FLAG_FRAME_SIZE_BYTES),
                       item.data,
                       LED_FLAG_FRAME_SIZE_BYTES);
                written = LED_FLAG_FRAME_SIZE_BYTES;
            } else {
                if (!s_rle_have_pending) {
                    memcpy(s_rle_last_frame, item.data, LED_FLAG_FRAME_SIZE_BYTES);
                    s_rle_repeat = 1;
                    s_rle_have_pending = true;
                    written = LED_FLAG_FRAME_SIZE_BYTES;
                } else if (memcmp(s_rle_last_frame, item.data, LED_FLAG_FRAME_SIZE_BYTES) == 0 &&
                           s_rle_repeat < UINT16_MAX) {
                    s_rle_repeat++;
                    written = LED_FLAG_FRAME_SIZE_BYTES;
                } else if (flush_rle_run_locked()) {
                    memcpy(s_rle_last_frame, item.data, LED_FLAG_FRAME_SIZE_BYTES);
                    s_rle_repeat = 1;
                    s_rle_have_pending = true;
                    written = LED_FLAG_FRAME_SIZE_BYTES;
                }
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (written == LED_FLAG_FRAME_SIZE_BYTES) {
                s_record_checksum = checksum32_update(s_record_checksum, item.data, LED_FLAG_FRAME_SIZE_BYTES);
                s_recorded_frames++;
                if (s_recorded_frames % 30 == 0) {
                    ESP_LOGI(TAG,
                             "Grabados %" PRIu32 " frames en %s checksum=0x%08" PRIx32,
                             s_recorded_frames,
                             s_record_path,
                             s_record_checksum);
                }
                if (s_record_max_frames > 0 && s_recorded_frames >= s_record_max_frames) {
                    ESP_LOGI(TAG,
                             "Grabacion alcanzo limite de %" PRIu32 " frames checksum=0x%08" PRIx32,
                             s_record_max_frames,
                             s_record_checksum);
                    bool header_ok = memory_mode ? true : flush_rle_run_locked();
                    if (!memory_mode) {
                        header_ok = update_record_header() && header_ok;
                    }
                    if (s_record_file != NULL) {
                        fclose(s_record_file);
                        s_record_file = NULL;
                    }
                    s_recording = false;
                    s_mem_recorded_frames = s_recorded_frames;
                    if (!memory_mode && !repair_closed_record_header()) {
                        ESP_LOGE(TAG,
                                 "No se pudo confirmar cabecera final (primera escritura=%u)",
                                 header_ok ? 1 : 0);
                    }
                }
            } else {
                s_record_dropped_frames++;
                ESP_LOGW(TAG, "Escritura SD corta: %u/%u", (unsigned)written, LED_FLAG_FRAME_SIZE_BYTES);
            }
            s_record_buffer_busy[item.buffer_index] = false;
            xSemaphoreGive(s_lock);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_record_buffer_busy[item.buffer_index] = false;
            xSemaphoreGive(s_lock);
        }
    }
}

static esp_err_t queue_record_frame_copy(const uint8_t *rgb555_frame, uint16_t frame_id)
{
    int buffer_index = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN; i++) {
        if (!s_record_buffer_busy[i]) {
            s_record_buffer_busy[i] = true;
            buffer_index = i;
            break;
        }
    }
    if (buffer_index < 0) {
        s_record_dropped_frames++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t *buffer = s_record_buffers[buffer_index];
    xSemaphoreGive(s_lock);

    memcpy(buffer, rgb555_frame, LED_FLAG_FRAME_SIZE_BYTES);
    record_item_t item = {
        .data = buffer,
        .frame_id = frame_id,
        .buffer_index = buffer_index,
    };
    if (xQueueSend(s_record_queue, &item, pdMS_TO_TICKS(5)) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_record_buffer_busy[buffer_index] = false;
        s_record_dropped_frames++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static bool record_buffers_idle(void)
{
    bool idle = uxQueueMessagesWaiting(s_record_queue) == 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN && idle; i++) {
        idle = !s_record_buffer_busy[i];
    }
    xSemaphoreGive(s_lock);
    return idle;
}

static bool read_show_header(FILE *file, sd_show_header_t *header)
{
    if (fread(header, 1, sizeof(*header), file) != sizeof(*header)) {
        return false;
    }
    return memcmp(header->magic, SD_SHOW_MAGIC, 4) == 0 &&
           header->version == SD_SHOW_VERSION &&
           header->pixel_count == LED_FLAG_TOTAL_LEDS &&
           header->output_count == LED_FLAG_OUTPUT_COUNT &&
           header->pixel_format == LED_FLAG_PIXEL_FORMAT_RGB555_BE &&
           header->pixels_per_output == LED_FLAG_LEDS_PER_OUTPUT &&
           header->frame_size == LED_FLAG_FRAME_SIZE_BYTES;
}

static void set_rgb555_pixel(uint8_t *frame, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= LED_FLAG_WIDTH || y < 0 || y >= LED_FLAG_HEIGHT) {
        return;
    }
    uint16_t rgb555 = ((uint16_t)(r >> 3) << 10) | ((uint16_t)(g >> 3) << 5) | (b >> 3);
    int offset = (y * LED_FLAG_WIDTH + x) * LED_FLAG_BYTES_PER_PIXEL;
    frame[offset] = (rgb555 >> 8) & 0xff;
    frame[offset + 1] = rgb555 & 0xff;
}

static uint8_t font_3x5(char ch, uint8_t row)
{
    static const uint8_t digits[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7},
        {0x2, 0x6, 0x2, 0x2, 0x7},
        {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7},
        {0x5, 0x5, 0x7, 0x1, 0x1},
        {0x7, 0x4, 0x7, 0x5, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7},
        {0x7, 0x1, 0x2, 0x2, 0x2},
        {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7},
    };
    if (row >= 5) {
        return 0;
    }
    if (ch >= '0' && ch <= '9') {
        return digits[ch - '0'][row];
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    switch (ch) {
    case 'E': return (const uint8_t[5]){0x7, 0x4, 0x6, 0x4, 0x7}[row];
    case 'N': return (const uint8_t[5]){0x5, 0x7, 0x7, 0x7, 0x5}[row];
    case 'O': return (const uint8_t[5]){0x7, 0x5, 0x5, 0x5, 0x7}[row];
    case 'R': return (const uint8_t[5]){0x6, 0x5, 0x6, 0x5, 0x5}[row];
    case 'C': return (const uint8_t[5]){0x7, 0x4, 0x4, 0x4, 0x7}[row];
    default: return 0;
    }
}

static void draw_text_3x5_scaled(uint8_t *frame,
                                 int x0,
                                 int y0,
                                 const char *text,
                                 int scale,
                                 uint8_t r,
                                 uint8_t g,
                                 uint8_t b)
{
    for (int i = 0; text[i] != '\0'; i++) {
        for (int y = 0; y < 5; y++) {
            uint8_t mask = font_3x5(text[i], y);
            for (int x = 0; x < 3; x++) {
                if ((mask & (1 << (2 - x))) == 0) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_rgb555_pixel(frame,
                                         x0 + i * (4 * scale) + x * scale + sx,
                                         y0 + y * scale + sy,
                                         r,
                                         g,
                                         b);
                    }
                }
            }
        }
    }
}

static void show_sd_label(const char *label, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *frame = heap_caps_calloc(1, LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGW(TAG, "Sin memoria para etiqueta SD");
        return;
    }

    for (int y = 0; y < LED_FLAG_HEIGHT; y++) {
        for (int x = 0; x < LED_FLAG_WIDTH; x++) {
            int output = y / LED_FLAG_ROWS_PER_OUTPUT;
            set_rgb555_pixel(frame,
                             x,
                             y,
                             output == 0 ? 12 : 0,
                             output == 1 ? 12 : 0,
                             output == 2 ? 12 : 0);
        }
    }

    int len = (int)strlen(label);
    int scale = 2;
    int text_w = len > 0 ? ((len * 4 - 1) * scale) : 0;
    draw_text_3x5_scaled(frame,
                         (LED_FLAG_WIDTH - text_w) / 2,
                         (LED_FLAG_HEIGHT - (5 * scale)) / 2,
                         label,
                         scale,
                         r,
                         g,
                         b);
    leds_commit_logical_frame(frame);
    free(frame);
}

static bool lfs_file_is_valid(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    sd_show_header_t header = {0};
    bool valid = read_show_header(file, &header) && header.frame_count > 0;
    if (valid) {
        if ((header.flags & SD_SHOW_FLAG_RLE_REPEAT_PREVIOUS) != 0) {
            uint32_t logical_frames = 0;
            while (logical_frames < header.frame_count) {
                sd_rle_record_header_t run = {0};
                if (fread(&run, 1, sizeof(run), file) != sizeof(run) ||
                    run.repeat == 0 ||
                    run.payload_size != header.frame_size ||
                    fseek(file, run.payload_size, SEEK_CUR) != 0) {
                    valid = false;
                    break;
                }
                logical_frames += run.repeat;
                if (logical_frames > header.frame_count) {
                    valid = false;
                    break;
                }
            }
            long end_pos = ftell(file);
            valid = valid && fseek(file, 0, SEEK_END) == 0 && ftell(file) == end_pos;
        } else {
            uint64_t expected_size = sizeof(header) +
                                     ((uint64_t)header.frame_count * header.frame_size);
            valid = fseek(file, 0, SEEK_END) == 0 &&
                    ftell(file) == (long)expected_size;
        }
    }
    fclose(file);
    return valid;
}

static void playback_task(void *arg)
{
    char path[96];
    uint16_t fps;
    bool clock_sync;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(path, s_record_path[0] != '\0' ? s_record_path : CONFIG_LED_FLAG_SD_DEFAULT_FILE, sizeof(path));
    fps = s_playback_fps > 0 ? s_playback_fps : CONFIG_LED_FLAG_SD_RECORD_FPS;
    clock_sync = s_playback_clock_sync;
    xSemaphoreGive(s_lock);

    uint8_t *frame = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Sin memoria para playback SD");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_playing = false;
        s_playback_task_handle = NULL;
        xSemaphoreGive(s_lock);
        vTaskDelete(NULL);
        return;
    }

    int64_t timeline_start_us = 0;
    uint64_t timeline_frame = 0;
    int64_t period_us = 0;
    int64_t worst_late_us = 0;
    uint32_t playback_checksum = 0;

    while (true) {
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            ESP_LOGE(TAG, "No se pudo abrir playback %s", path);
            break;
        }

        sd_show_header_t header = {0};
        if (!read_show_header(file, &header)) {
            ESP_LOGE(TAG, "Archivo show invalido: %s", path);
            fclose(file);
            break;
        }
        if (header.fps > 0) {
            fps = header.fps;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_playback_file_fps = fps;
        xSemaphoreGive(s_lock);
        bool rle_file = (header.flags & SD_SHOW_FLAG_RLE_REPEAT_PREVIOUS) != 0;
        ESP_LOGI(TAG,
                 "Reproduciendo %s frames=%" PRIu32 " fps=%u sync_reloj=%u rle=%u",
                 path,
                 header.frame_count,
                 fps,
                 clock_sync ? 1 : 0,
                 rle_file ? 1 : 0);

        const uint32_t safe_fps = fps > 0 ? fps : 30;
        if (period_us == 0) {
            period_us = (1000000LL + (safe_fps / 2)) / safe_fps;
        }
        const TickType_t fallback_period = pdMS_TO_TICKS(1000 / safe_fps);
        TickType_t next_wake = xTaskGetTickCount();
        if (timeline_start_us == 0) {
            timeline_start_us = esp_timer_get_time();
        }
        uint32_t played = 0;

        while (played < header.frame_count) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool playing = s_playing;
            xSemaphoreGive(s_lock);
            if (!playing) {
                fclose(file);
                goto out;
            }

            uint16_t repeat = 1;
            if (rle_file) {
                sd_rle_record_header_t run = {0};
                if (fread(&run, 1, sizeof(run), file) != sizeof(run) ||
                    run.repeat == 0 ||
                    run.payload_size != LED_FLAG_FRAME_SIZE_BYTES ||
                    fread(frame, 1, LED_FLAG_FRAME_SIZE_BYTES, file) != LED_FLAG_FRAME_SIZE_BYTES) {
                    ESP_LOGW(TAG, "Fin inesperado de archivo SD RLE");
                    break;
                }
                repeat = run.repeat;
            } else if (fread(frame, 1, LED_FLAG_FRAME_SIZE_BYTES, file) != LED_FLAG_FRAME_SIZE_BYTES) {
                ESP_LOGW(TAG, "Fin inesperado de archivo SD");
                break;
            }
            for (uint16_t r = 0; r < repeat && played < header.frame_count; ++r) {
                if (clock_sync) {
                    int64_t target_us = timeline_start_us + ((int64_t)timeline_frame * period_us);
                    while (true) {
                        int64_t now_us = esp_timer_get_time();
                        int64_t wait_us = target_us - now_us;
                        if (wait_us <= 0) {
                            int64_t late_us = -wait_us;
                            if (late_us > worst_late_us) {
                                worst_late_us = late_us;
                            }
                            break;
                        }
                        if (wait_us > 2000) {
                            vTaskDelay(pdMS_TO_TICKS(1));
                        } else {
                            esp_rom_delay_us((uint32_t)wait_us);
                        }
                    }
                }
                playback_checksum = checksum32_update(playback_checksum, frame, LED_FLAG_FRAME_SIZE_BYTES);
                leds_commit_frame(frame);
                played++;
                timeline_frame++;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_playback_frames = (uint32_t)timeline_frame;
                xSemaphoreGive(s_lock);
                if (!clock_sync) {
                    vTaskDelayUntil(&next_wake, fallback_period);
                }
            }
        }

        ESP_LOGI(TAG,
                 "Playback vuelta terminada: played=%" PRIu32 " timeline=%" PRIu64
                 " checksum=0x%08" PRIx32 " peor_atraso_us=%lld",
                 played,
                 timeline_frame,
                 playback_checksum,
                 (long long)worst_late_us);

        fclose(file);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool loop = s_playback_loop && s_playing;
        s_playback_loops++;
        xSemaphoreGive(s_lock);
        if (!loop) {
            break;
        }
    }

out:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_playback_checksum = playback_checksum;
    xSemaphoreGive(s_lock);
    free(frame);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_playing = false;
    s_playback_task_handle = NULL;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Playback SD detenido");
    vTaskDelete(NULL);
}

static void memory_playback_task(void *arg)
{
    (void)arg;
    uint16_t fps;
    bool clock_sync;
    bool loop;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fps = s_playback_fps > 0 ? s_playback_fps : CONFIG_LED_FLAG_SD_RECORD_FPS;
    clock_sync = s_playback_clock_sync;
    loop = s_playback_loop;
    xSemaphoreGive(s_lock);

    uint8_t *frame = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_playing = false;
        s_playback_task_handle = NULL;
        xSemaphoreGive(s_lock);
        vTaskDelete(NULL);
        return;
    }

    uint32_t checksum = 0;
    uint64_t timeline_frame = 0;
    int64_t start_us = esp_timer_get_time();
    const uint16_t safe_fps = fps > 0 ? fps : 30;
    const int64_t period_us = (1000000LL + safe_fps / 2) / safe_fps;
    const TickType_t fallback_period = pdMS_TO_TICKS(1000 / safe_fps);
    do {
        for (uint32_t i = 0; i < s_mem_recorded_frames; ++i) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool playing = s_playing;
            xSemaphoreGive(s_lock);
            if (!playing) goto done;
            memcpy(frame, s_mem_record + (i * LED_FLAG_FRAME_SIZE_BYTES), LED_FLAG_FRAME_SIZE_BYTES);
            if (clock_sync) {
                int64_t target_us = start_us + (int64_t)timeline_frame * period_us;
                while (esp_timer_get_time() < target_us) {
                    esp_rom_delay_us(50);
                }
            }
            checksum = checksum32_update(checksum, frame, LED_FLAG_FRAME_SIZE_BYTES);
            leds_commit_frame(frame);
            timeline_frame++;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_playback_frames = (uint32_t)timeline_frame;
            xSemaphoreGive(s_lock);
            if (!clock_sync) {
                vTaskDelay(fallback_period);
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_playback_loops++;
        loop = s_playback_loop && s_playing;
        xSemaphoreGive(s_lock);
    } while (loop);

done:
    free(frame);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_playback_checksum = checksum;
    s_playing = false;
    s_playback_task_handle = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t sd_storage_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "sin mutex SD");
    }
    if (s_playback_control_lock == NULL) {
        s_playback_control_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_playback_control_lock != NULL, ESP_ERR_NO_MEM, TAG, "sin mutex playback SD");
    }
    if (s_record_queue == NULL) {
        s_record_queue = xQueueCreate(CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN, sizeof(record_item_t));
        ESP_RETURN_ON_FALSE(s_record_queue != NULL, ESP_ERR_NO_MEM, TAG, "sin cola SD");
    }
    for (int i = 0; i < CONFIG_LED_FLAG_SD_RECORD_QUEUE_LEN; i++) {
        if (s_record_buffers[i] == NULL) {
            s_record_buffers[i] = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
            ESP_RETURN_ON_FALSE(s_record_buffers[i] != NULL, ESP_ERR_NO_MEM, TAG, "sin buffer SD");
        }
    }
    if (s_rle_last_frame == NULL) {
        s_rle_last_frame = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_rle_last_frame != NULL, ESP_ERR_NO_MEM, TAG, "sin buffer RLE SD");
    }
    if (s_record_task_handle == NULL) {
        xTaskCreate(record_task, "sd_record", 4096, NULL, 6, &s_record_task_handle);
    }

    ESP_LOGI(TAG,
             "Montando microSD SPI CS=%d SCK=%d MISO=%d MOSI=%d en %s",
             runtime_config_get()->sd_cs_gpio,
             runtime_config_get()->sd_sclk_gpio,
             runtime_config_get()->sd_miso_gpio,
             runtime_config_get()->sd_mosi_gpio,
             CONFIG_LED_FLAG_SD_MOUNT_POINT);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 10000;
    spi_bus_config_t bus_config = {
        .mosi_io_num = runtime_config_get()->sd_mosi_gpio,
        .miso_io_num = runtime_config_get()->sd_miso_gpio,
        .sclk_io_num = runtime_config_get()->sd_sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    esp_err_t bus_ret = spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        strlcpy(s_last_error, esp_err_to_name(bus_ret), sizeof(s_last_error));
        return bus_ret;
    }
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = runtime_config_get()->sd_cs_gpio;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(CONFIG_LED_FLAG_SD_MOUNT_POINT,
                                            &host,
                                            &slot_config,
                                            &mount_config,
                                            &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo montar SD: %s", esp_err_to_name(ret));
        strlcpy(s_last_error, esp_err_to_name(ret), sizeof(s_last_error));
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    strlcpy(s_last_error, "OK", sizeof(s_last_error));
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "microSD lista");
    return ESP_OK;
}

bool sd_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_storage_start_record(const char *path, uint32_t max_frames, uint16_t fps, bool show_leds)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "path invalido");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_recording) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool memory_mode = !s_mounted && runtime_config_get()->record_to_memory_if_no_sd;
    FILE *file = NULL;
    if (!memory_mode) {
        if (!s_mounted) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "SD no montada y fallback memoria desactivado");
            return ESP_ERR_INVALID_STATE;
        }
        file = fopen(path, "wb");
    }
    if (file == NULL) {
        if (memory_mode) {
            uint32_t requested = max_frames > 0 ? max_frames : MEMORY_RECORD_DEFAULT_FRAMES;
            if (requested > MEMORY_RECORD_DEFAULT_FRAMES) {
                requested = MEMORY_RECORD_DEFAULT_FRAMES;
            }
            size_t bytes = requested * LED_FLAG_FRAME_SIZE_BYTES;
            free(s_mem_record);
            s_mem_record = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
            if (s_mem_record == NULL) {
                xSemaphoreGive(s_lock);
                ESP_LOGE(TAG, "Sin RAM para grabacion memoria (%u frames)", (unsigned)requested);
                return ESP_ERR_NO_MEM;
            }
            s_mem_capacity_frames = requested;
            s_mem_recorded_frames = 0;
            file = NULL;
        } else {
        int err = errno;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "No se pudo crear %s errno=%d (%s)", path, err, strerror(err));
        return ESP_FAIL;
        }
    }

    if (!memory_mode && !write_initial_header(file,
                              fps > 0 ? fps : CONFIG_LED_FLAG_SD_RECORD_FPS,
                              max_frames)) {
        int err = errno;
        fclose(file);
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "No se pudo escribir cabecera inicial %s errno=%d", path, err);
        return ESP_FAIL;
    }
    s_record_file = file;
    strlcpy(s_record_path, memory_mode ? MEMORY_RECORD_PATH : path, sizeof(s_record_path));
    s_recorded_frames = 0;
    s_record_dropped_frames = 0;
    s_record_max_frames = max_frames;
    s_record_checksum = 0;
    s_rle_have_pending = false;
    s_rle_repeat = 0;
    s_recorded_runs = 0;
    s_record_sequence_gaps = 0;
    s_record_fps = fps > 0 ? fps : CONFIG_LED_FLAG_SD_RECORD_FPS;
    s_record_show_leds = show_leds;
    s_record_have_frame_id = false;
    s_record_last_frame_id = 0;
    s_record_stopping = false;
    s_mem_mode = memory_mode;
    memset(s_record_buffer_busy, 0, sizeof(s_record_buffer_busy));
    xQueueReset(s_record_queue);
    s_recording = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG,
             "Grabacion SD iniciada: %s max_frames=%" PRIu32 " show_leds=%u",
             s_record_path,
             max_frames,
             show_leds ? 1 : 0);
    return ESP_OK;
}

esp_err_t sd_storage_stop_record(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_recording && s_record_file == NULL) {
        bool repaired = s_recorded_frames == 0 || repair_closed_record_header();
        xSemaphoreGive(s_lock);
        return repaired ? ESP_OK : ESP_FAIL;
    }
    s_record_stopping = true;
    xSemaphoreGive(s_lock);

    int waited_ms = 0;
    while (!record_buffers_idle() && waited_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    bool memory_mode = s_mem_mode;
    bool header_ok = memory_mode ? true : flush_rle_run_locked();
    if (!memory_mode) {
        header_ok = update_record_header() && header_ok;
    }
    if (s_record_file != NULL) {
        fclose(s_record_file);
        s_record_file = NULL;
    }
    if (memory_mode) {
        s_mem_recorded_frames = s_recorded_frames;
    } else {
        header_ok = repair_closed_record_header();
    }
    ESP_LOGI(TAG,
             "Grabacion SD detenida: frames=%" PRIu32 " drop=%" PRIu32
             " sequence_gaps=%" PRIu32 " drain_ms=%d checksum=0x%08" PRIx32,
             s_recorded_frames,
             s_record_dropped_frames,
             s_record_sequence_gaps,
             waited_ms,
             s_record_checksum);
    s_record_stopping = false;
    s_mem_mode = false;
    xSemaphoreGive(s_lock);
    return header_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_storage_record_frame(const uint8_t *rgb555_frame, uint16_t frame_id)
{
    if (rgb555_frame == NULL || !s_recording || s_record_stopping) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have_previous = s_record_have_frame_id;
    uint16_t previous_id = s_record_last_frame_id;
    xSemaphoreGive(s_lock);

    if (have_previous) {
        uint16_t delta = (uint16_t)(frame_id - previous_id);
        if (delta == 0 || delta > 0x7fff) {
            return ESP_OK;
        }
        uint16_t missing = delta - 1;
        if (missing > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_record_sequence_gaps += missing;
            xSemaphoreGive(s_lock);
        }
    }

    esp_err_t ret = queue_record_frame_copy(rgb555_frame, frame_id);
    if (ret == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_record_have_frame_id = true;
        s_record_last_frame_id = frame_id;
        xSemaphoreGive(s_lock);
    }
    return ret;
}

bool sd_storage_is_playing(void)
{
    return s_playing;
}

void sd_storage_upload_abort(void)
{
    if (s_upload_file != NULL) {
        fclose(s_upload_file);
        s_upload_file = NULL;
    }
    if (s_upload_path[0] != '\0') {
        unlink(s_upload_path);
    }
    s_upload_path[0] = '\0';
    s_upload_expected_size = 0;
    s_upload_expected_checksum = 0;
    s_upload_received = 0;
    s_upload_checksum = 0;
}

esp_err_t sd_storage_upload_begin(const char *path, uint32_t file_size, uint32_t checksum)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "SD no montada");
    ESP_RETURN_ON_FALSE(path != NULL && strncmp(path, "/sdcard/rec", 11) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "ruta upload invalida");
    ESP_RETURN_ON_FALSE(file_size >= sizeof(sd_show_header_t),
                        ESP_ERR_INVALID_SIZE, TAG, "archivo LFS demasiado pequeno");

    sd_storage_stop_record();
    sd_storage_stop_playback();
    sd_storage_upload_abort();

    s_upload_file = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(s_upload_file != NULL, ESP_FAIL, TAG, "no se pudo crear %s", path);
    strlcpy(s_upload_path, path, sizeof(s_upload_path));
    s_upload_expected_size = file_size;
    s_upload_expected_checksum = checksum;
    ESP_LOGI(TAG, "Upload SD BEGIN path=%s size=%" PRIu32 " checksum=0x%08" PRIx32,
             path, file_size, checksum);
    return ESP_OK;
}

esp_err_t sd_storage_upload_write(uint32_t offset, const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(s_upload_file != NULL, ESP_ERR_INVALID_STATE, TAG, "upload no activo");
    ESP_RETURN_ON_FALSE(data != NULL && size > 0, ESP_ERR_INVALID_ARG, TAG, "chunk vacio");
    if (offset < s_upload_received && offset + size <= s_upload_received) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(offset == s_upload_received, ESP_ERR_INVALID_ARG, TAG,
                        "offset upload got=%" PRIu32 " expected=%" PRIu32,
                        offset, s_upload_received);
    ESP_RETURN_ON_FALSE(s_upload_received + size <= s_upload_expected_size,
                        ESP_ERR_INVALID_SIZE, TAG, "upload excede tamano");

    size_t written = fwrite(data, 1, size, s_upload_file);
    ESP_RETURN_ON_FALSE(written == size, ESP_FAIL, TAG, "escritura upload corta");
    s_upload_checksum = checksum32_update(s_upload_checksum, data, size);
    s_upload_received += size;
    return ESP_OK;
}

esp_err_t sd_storage_upload_end(void)
{
    ESP_RETURN_ON_FALSE(s_upload_file != NULL, ESP_ERR_INVALID_STATE, TAG, "upload no activo");
    fflush(s_upload_file);
    fclose(s_upload_file);
    s_upload_file = NULL;

    bool valid = s_upload_received == s_upload_expected_size &&
                 s_upload_checksum == s_upload_expected_checksum &&
                 lfs_file_is_valid(s_upload_path);
    if (!valid) {
        ESP_LOGE(TAG,
                 "Upload SD invalido bytes=%" PRIu32 "/%" PRIu32
                 " checksum=0x%08" PRIx32 "/0x%08" PRIx32,
                 s_upload_received, s_upload_expected_size,
                 s_upload_checksum, s_upload_expected_checksum);
        s_recorded_frames = s_upload_received;
        s_record_dropped_frames = s_upload_checksum;
        strlcpy(s_record_path, s_upload_path, sizeof(s_record_path));
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "Upload SD OK path=%s bytes=%" PRIu32 " checksum=0x%08" PRIx32,
             s_upload_path, s_upload_received, s_upload_checksum);
    s_recorded_frames = s_upload_received;
    s_record_dropped_frames = 0;
    strlcpy(s_record_path, s_upload_path, sizeof(s_record_path));
    s_upload_path[0] = '\0';
    return ESP_OK;
}

bool sd_storage_is_recording(void)
{
    bool recording;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    recording = s_recording;
    xSemaphoreGive(s_lock);
    return recording;
}

bool sd_storage_record_show_leds(void)
{
    bool show_leds;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    show_leds = s_record_show_leds;
    xSemaphoreGive(s_lock);
    return show_leds;
}

esp_err_t sd_storage_start_playback(const char *path, uint16_t fps, bool loop, bool clock_sync)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "path invalido");
    bool memory_playback = strcmp(path, MEMORY_RECORD_PATH) == 0 ||
                           (!s_mounted && s_mem_record != NULL && s_mem_recorded_frames > 0);
    if (!memory_playback) {
        ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "SD no montada");
        ESP_RETURN_ON_FALSE(lfs_file_is_valid(path), ESP_ERR_NOT_FOUND, TAG, "archivo LFS ausente o invalido");
    }

    xSemaphoreTake(s_playback_control_lock, portMAX_DELAY);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same_playback = s_playing &&
                         s_playback_task_handle != NULL &&
                         strcmp(s_record_path, path) == 0 &&
                         s_playback_fps == fps &&
                         s_playback_loop == loop &&
                         s_playback_clock_sync == clock_sync;
    if (same_playback) {
        s_playback_duplicate_starts++;
    }
    xSemaphoreGive(s_lock);
    if (same_playback) {
        xSemaphoreGive(s_playback_control_lock);
        ESP_LOGW(TAG, "Playback SD ya activo; orden duplicada ignorada: %s", path);
        return ESP_OK;
    }

    esp_err_t ret = sd_storage_stop_record();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_playback_control_lock);
        return ret;
    }
    ret = stop_playback_task_and_wait();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_playback_control_lock);
        return ret;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_record_path, path, sizeof(s_record_path));
    s_playing = true;
    s_playback_memory = memory_playback;
    s_playback_loop = loop;
    s_playback_fps = fps;
    s_playback_clock_sync = clock_sync;
    s_playback_file_fps = 0;
    s_playback_frames = 0;
    s_playback_loops = 0;
    s_playback_starts++;
    BaseType_t created = xTaskCreate(memory_playback ? memory_playback_task : playback_task,
                                     memory_playback ? "mem_playback" : "sd_playback",
                                     4096,
                                     NULL,
                                     3,
                                     &s_playback_task_handle);
    if (created != pdPASS) {
        s_playing = false;
        s_playback_task_handle = NULL;
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_playback_control_lock);

    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "no se pudo crear tarea playback SD");
    ESP_LOGI(TAG, "Playback %s solicitado: %s fps=%u loop=%d sync_reloj=%d",
             memory_playback ? "RAM" : "SD", path, fps, loop, clock_sync);
    return ESP_OK;
}

esp_err_t sd_storage_play_next_rec(uint16_t fps, bool loop, bool clock_sync)
{
    if (!s_mounted && s_mem_record != NULL && s_mem_recorded_frames > 0) {
        return sd_storage_start_playback(MEMORY_RECORD_PATH, fps, loop, clock_sync);
    }
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "SD no montada");

    sd_storage_stop_record();
    sd_storage_stop_playback();
    vTaskDelay(pdMS_TO_TICKS(120));

    for (int tries = 0; tries < 10; tries++) {
        int candidate = (s_next_rec_index % 10) + 1;
        s_next_rec_index = candidate;

        char path[96];
        snprintf(path, sizeof(path), CONFIG_LED_FLAG_SD_MOUNT_POINT "/rec%d.lfs", candidate);
        if (!lfs_file_is_valid(path)) {
            ESP_LOGW(TAG, "Show rental no existe o invalido, saltando: %s", path);
            continue;
        }

        char label[8];
        snprintf(label, sizeof(label), "REC%d", candidate);
        ESP_LOGI(TAG, "Show rental seleccionado: %s", path);
        show_sd_label(label, 255, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return sd_storage_start_playback(path, fps, loop, clock_sync);
    }

    ESP_LOGW(TAG, "No hay archivos rental validos rec1.lfs..rec10.lfs");
    show_sd_label("NO REC", 255, 80, 40);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sd_storage_delete_recs(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "SD no montada");

    sd_storage_stop_record();
    sd_storage_stop_playback();
    vTaskDelay(pdMS_TO_TICKS(120));

    int removed = 0;
    int errors = 0;
    for (int i = 1; i <= 10; i++) {
        char path[96];
        snprintf(path, sizeof(path), CONFIG_LED_FLAG_SD_MOUNT_POINT "/rec%d.lfs", i);
        if (unlink(path) == 0) {
            removed++;
            ESP_LOGI(TAG, "Archivo borrado: %s", path);
        } else if (errno != ENOENT) {
            errors++;
            ESP_LOGW(TAG, "No se pudo borrar %s errno=%d", path, errno);
        }
    }

    s_next_rec_index = 0;
    leds_clear();
    ESP_LOGI(TAG, "Borrado rec1..rec10 completo: removed=%d errors=%d", removed, errors);
    return errors == 0 ? ESP_OK : ESP_FAIL;
}

uint32_t sd_storage_list_recs(char *buffer, size_t buffer_size)
{
    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (!s_mounted) {
        return 0;
    }

    uint32_t mask = 0;
    size_t used = 0;
    for (int i = 1; i <= 10; i++) {
        char path[96];
        snprintf(path, sizeof(path), CONFIG_LED_FLAG_SD_MOUNT_POINT "/rec%d.lfs", i);
        if (!lfs_file_is_valid(path)) {
            continue;
        }
        mask |= (1UL << (i - 1));
        if (buffer != NULL && buffer_size > 0) {
            int written = snprintf(buffer + used,
                                   used < buffer_size ? buffer_size - used : 0,
                                   "%srec%d.lfs",
                                   used > 0 ? "," : "",
                                   i);
            if (written > 0) {
                used += (size_t)written;
                if (used >= buffer_size) {
                    buffer[buffer_size - 1] = '\0';
                    break;
                }
            }
        }
    }
    ESP_LOGI(TAG, "Lista rec SD mask=0x%08" PRIx32 " files=%s", mask, buffer != NULL ? buffer : "");
    return mask;
}

esp_err_t sd_storage_stop_playback(void)
{
    ESP_RETURN_ON_FALSE(s_playback_control_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "playback no inicializado");
    xSemaphoreTake(s_playback_control_lock, portMAX_DELAY);
    esp_err_t ret = stop_playback_task_and_wait();
    xSemaphoreGive(s_playback_control_lock);
    return ret;
}

void sd_storage_get_status(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buffer,
             buffer_size,
             "\"sd_mounted\":%s,\"recording\":%s,\"playing\":%s,"
             "\"sd_queue_depth\":%u,"
             "\"recorded_frames\":%" PRIu32 ",\"record_dropped_frames\":%" PRIu32 ","
             "\"record_checksum\":%" PRIu32 ",\"playback_checksum\":%" PRIu32 ","
             "\"memory_record_available\":%s,\"memory_record_frames\":%" PRIu32 ",\"storage\":\"%s\","
             "\"playback_fps\":%u,\"playback_frames\":%" PRIu32 ",\"playback_loops\":%" PRIu32 ","
             "\"playback_starts\":%" PRIu32 ",\"playback_duplicate_starts\":%" PRIu32 ","
             "\"file\":\"%s\",\"sd_last_error\":\"%s\"",
             s_mounted ? "true" : "false",
             s_recording ? "true" : "false",
             s_playing ? "true" : "false",
             s_record_queue ? (unsigned)uxQueueMessagesWaiting(s_record_queue) : 0,
             s_recorded_frames,
             s_record_dropped_frames,
             s_record_checksum,
             s_last_playback_checksum,
             s_mem_recorded_frames > 0 ? "true" : "false",
             s_mem_recorded_frames,
             s_mem_mode || s_playback_memory ? "memory" : "sd",
             s_playback_file_fps > 0 ? s_playback_file_fps : s_playback_fps,
             s_playback_frames,
             s_playback_loops,
             s_playback_starts,
             s_playback_duplicate_starts,
             s_record_path,
             s_last_error);
    xSemaphoreGive(s_lock);
}

void sd_storage_get_status_values(bool *mounted,
                                  bool *recording,
                                  bool *playing,
                                  uint32_t *recorded_frames,
                                  uint32_t *record_dropped_frames,
                                  char *path,
                                  size_t path_size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mounted != NULL) {
        *mounted = s_mounted;
    }
    if (recording != NULL) {
        *recording = s_recording;
    }
    if (playing != NULL) {
        *playing = s_playing;
    }
    if (recorded_frames != NULL) {
        *recorded_frames = s_recorded_frames;
    }
    if (record_dropped_frames != NULL) {
        *record_dropped_frames = s_record_dropped_frames;
    }
    if (path != NULL && path_size > 0) {
        strlcpy(path, s_record_path, path_size);
    }
    xSemaphoreGive(s_lock);
}

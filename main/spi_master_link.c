#include "spi_master_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "leds.h"

/* Compatibility facade: callers and HTTP routes keep the proven C5 API, but
 * operations are now local. There is no CAM/WROOM transport in this variant. */
static const char *TAG = "local_backend";
static volatile bool s_pause_frames;
static spi_link_sd_status_t s_last_status;
static bool s_status_valid;
static bool s_last_status_is_list;

static void refresh_status(void)
{
    char path[SPI_LINK_SD_PATH_MAX] = {0};
    s_last_status = (spi_link_sd_status_t) {
        .magic = SPI_LINK_SD_STATUS_MAGIC,
        .version = SPI_LINK_VERSION,
        .status = SPI_LINK_STATUS_COMMAND_OK,
        .sd_mounted = false,
        .recording = false,
        .playing = false,
        .recorded_frames = 0,
        .record_dropped_frames = 0,
    };
    strlcpy(s_last_status.file, path, sizeof(s_last_status.file));
    s_status_valid = true;
    s_last_status_is_list = false;
}

esp_err_t spi_master_link_init(void)
{
    ESP_RETURN_ON_ERROR(leds_init(), TAG, "salidas LED");
    leds_start_task();
    refresh_status();
    ESP_LOGI(TAG, "Backend local C5 listo: SD desactivada, sin CAM ni enlace SPI intermedio");
    return ESP_OK;
}

void spi_master_link_start_task(void) { }

esp_err_t spi_master_link_queue_frame(uint16_t frame_id, const uint8_t *frame)
{
    return spi_master_link_queue_frame_with_brightness(frame_id, frame, false, 255);
}

esp_err_t spi_master_link_queue_frame_with_brightness(uint16_t frame_id, const uint8_t *frame,
                                                       bool brightness_limit_enabled, uint8_t brightness_limit)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    (void)frame_id;
    if (s_pause_frames) return ESP_ERR_INVALID_STATE;
    return leds_commit_frame_with_brightness(frame, brightness_limit_enabled, brightness_limit);
}

esp_err_t spi_master_link_send_command(uint8_t command, uint32_t value)
{
    (void)value;
    if (command == SPI_LINK_CMD_REBOOT) {
        esp_restart();
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t spi_master_link_send_sd_control(uint8_t command, const char *path,
                                          uint32_t frames, uint16_t fps, bool loop,
                                          bool record_show_leds, bool clock_sync)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    switch (command) {
    case SPI_LINK_CMD_SD_RECORD_START:
    case SPI_LINK_CMD_SD_RECORD_STOP:
    case SPI_LINK_CMD_SD_PLAY_START:
    case SPI_LINK_CMD_SD_PLAY_STOP:
    case SPI_LINK_CMD_SD_PLAY_NEXT_REC:
    case SPI_LINK_CMD_SD_DELETE_RECS:
        (void)path; (void)frames; (void)fps; (void)loop; (void)record_show_leds; (void)clock_sync;
        ret = ESP_ERR_NOT_SUPPORTED; break;
    case SPI_LINK_CMD_SD_LIST_RECS: {
        char files[96] = "";
        uint32_t count = 0;
        refresh_status();
        strlcpy(s_last_status.file, files, sizeof(s_last_status.file));
        s_last_status.recorded_frames = 0;
        s_last_status.record_dropped_frames = count;
        s_last_status_is_list = true;
        return ESP_OK;
    }
    case SPI_LINK_CMD_SD_STATUS:
        ret = ESP_OK; break;
    default:
        ret = ESP_ERR_NOT_SUPPORTED; break;
    }
    refresh_status();
    return ret;
}

esp_err_t spi_master_link_get_sd_status(spi_link_sd_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    refresh_status();
    *status = s_last_status;
    return ESP_ERR_INVALID_STATE;
}

bool spi_master_link_last_sd_status(spi_link_sd_status_t *status)
{
    if (!status || !s_status_valid) return false;
    if (!s_last_status_is_list) {
        refresh_status();
    }
    *status = s_last_status;
    return true;
}

esp_err_t spi_master_link_send_ota_chunk(uint32_t offset, const uint8_t *data,
                                         uint16_t size, uint32_t image_size)
{
    (void)offset; (void)data; (void)size; (void)image_size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t spi_master_link_send_sd_upload_control(uint8_t command, const char *path,
                                                 uint32_t file_size, uint32_t checksum)
{
    switch (command) {
    case SPI_LINK_CMD_SD_UPLOAD_BEGIN:
    case SPI_LINK_CMD_SD_UPLOAD_END:
    case SPI_LINK_CMD_SD_UPLOAD_ABORT:
        (void)path; (void)file_size; (void)checksum;
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t spi_master_link_send_sd_file_chunk(uint32_t offset, const uint8_t *data,
                                             uint16_t size, uint32_t file_size)
{
    (void)file_size;
    (void)offset; (void)data; (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

void spi_master_link_set_frame_pause(bool paused) { s_pause_frames = paused; }
void spi_master_link_set_sd_activity(bool recording, bool playing) { (void)recording; (void)playing; refresh_status(); }
uint32_t spi_master_link_sent_frames(void) { return leds_output_frames(); }
uint32_t spi_master_link_dropped_frames(void) { return leds_dropped_frames(); }
bool spi_master_link_frames_paused(void) { return s_pause_frames; }
bool spi_master_link_sd_recording(void) { return false; }
bool spi_master_link_sd_playing(void) { return false; }

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "spi_link_protocol.h"

esp_err_t spi_master_link_init(void);
void spi_master_link_start_task(void);
esp_err_t spi_master_link_queue_frame(uint16_t frame_id, const uint8_t *rgb555_frame);
esp_err_t spi_master_link_queue_frame_with_brightness(uint16_t frame_id, const uint8_t *rgb555_frame,
                                                       bool brightness_limit_enabled, uint8_t brightness_limit);
esp_err_t spi_master_link_send_command(uint8_t command, uint32_t value);
esp_err_t spi_master_link_send_sd_control(uint8_t command,
                                          const char *path,
                                          uint32_t frames,
                                          uint16_t fps,
                                          bool loop,
                                          bool record_show_leds,
                                          bool clock_sync);
esp_err_t spi_master_link_get_sd_status(spi_link_sd_status_t *status);
bool spi_master_link_last_sd_status(spi_link_sd_status_t *status);
esp_err_t spi_master_link_send_ota_chunk(uint32_t offset,
                                         const uint8_t *data,
                                         uint16_t size,
                                         uint32_t image_size);
esp_err_t spi_master_link_send_sd_upload_control(uint8_t command,
                                                 const char *path,
                                                 uint32_t file_size,
                                                 uint32_t checksum);
esp_err_t spi_master_link_send_sd_file_chunk(uint32_t offset,
                                             const uint8_t *data,
                                             uint16_t size,
                                             uint32_t file_size);
void spi_master_link_set_frame_pause(bool paused);
void spi_master_link_set_sd_activity(bool recording, bool playing);
uint32_t spi_master_link_sent_frames(void);
uint32_t spi_master_link_dropped_frames(void);
bool spi_master_link_frames_paused(void);
bool spi_master_link_sd_recording(void);
bool spi_master_link_sd_playing(void);

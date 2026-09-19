#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sd_storage_init(void);
bool sd_storage_is_mounted(void);
esp_err_t sd_storage_start_record(const char *path, uint32_t max_frames, uint16_t fps, bool show_leds);
esp_err_t sd_storage_stop_record(void);
esp_err_t sd_storage_record_frame(const uint8_t *rgb555_frame, uint16_t frame_id);
bool sd_storage_is_recording(void);
bool sd_storage_is_playing(void);
bool sd_storage_record_show_leds(void);
esp_err_t sd_storage_start_playback(const char *path, uint16_t fps, bool loop, bool clock_sync);
esp_err_t sd_storage_play_next_rec(uint16_t fps, bool loop, bool clock_sync);
esp_err_t sd_storage_stop_playback(void);
esp_err_t sd_storage_delete_recs(void);
esp_err_t sd_storage_upload_begin(const char *path, uint32_t file_size, uint32_t checksum);
esp_err_t sd_storage_upload_write(uint32_t offset, const uint8_t *data, size_t size);
esp_err_t sd_storage_upload_end(void);
void sd_storage_upload_abort(void);
uint32_t sd_storage_list_recs(char *buffer, size_t buffer_size);
void sd_storage_get_status(char *buffer, size_t buffer_size);
void sd_storage_get_status_values(bool *mounted,
                                  bool *recording,
                                  bool *playing,
                                  uint32_t *recorded_frames,
                                  uint32_t *record_dropped_frames,
                                  char *path,
                                  size_t path_size);

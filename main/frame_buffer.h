#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t frame_buffer_init(void);
uint8_t *frame_buffer_get_receive_buffer(void);
esp_err_t frame_buffer_commit_receive_buffer(uint16_t frame_id, bool brightness_limit_enabled,
                                             uint8_t brightness_limit);
esp_err_t frame_buffer_copy_latest(uint8_t *destination, size_t size);
bool frame_buffer_has_complete_frame(void);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t fallback_image_init(void);
esp_err_t fallback_image_capture_latest(void);
esp_err_t fallback_image_clear(void);
esp_err_t fallback_image_copy(uint8_t *destination, size_t size);
bool fallback_image_is_available(void);
uint32_t fallback_image_checksum(void);
void fallback_image_status_json(char *buffer, size_t size);

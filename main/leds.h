#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t leds_init(void);
void leds_start_task(void);
esp_err_t leds_commit_frame(const uint8_t *rgb555_frame);
esp_err_t leds_commit_frame_with_brightness(const uint8_t *rgb555_frame, bool enabled, uint8_t limit);
bool leds_brightness_limit_active(void);
uint8_t leds_brightness_limit_value(void);
esp_err_t leds_commit_logical_frame(const uint8_t *rgb555_frame);
esp_err_t leds_clear(void);
void leds_show_frame(const uint8_t *rgb555_frame);
void leds_show_test_pattern(void);
uint32_t leds_output_frames(void);
uint32_t leds_dropped_frames(void);
uint32_t leds_last_output_us(void);
uint32_t leds_output_fps(void);

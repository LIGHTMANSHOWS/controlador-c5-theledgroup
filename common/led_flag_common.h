#pragma once

#include <stdint.h>

/* Shared maximum geometry for the single-board ESP32-C5 variant.
 * Runtime configuration selects the useful count for each output. */
#define LED_FLAG_MAX_PIXELS_PER_OUTPUT 600
#define LED_FLAG_WIDTH                 600
#define LED_FLAG_HEIGHT                6
#define LED_FLAG_OUTPUT_COUNT          6
#define LED_FLAG_ROWS_PER_OUTPUT       1
#define LED_FLAG_LEDS_PER_OUTPUT       600
#define LED_FLAG_PHYSICAL_LEDS_PER_OUTPUT 601
#define LED_FLAG_TOTAL_LEDS            1200 /* total maximum per C5 controller */
#define LED_FLAG_BYTES_PER_PIXEL       2
#define LED_FLAG_FRAME_SIZE_BYTES      2400
#define LED_FLAG_ROW_SIZE_BYTES        1200
#define LED_FLAG_PROTOCOL_VERSION      2

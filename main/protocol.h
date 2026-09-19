#pragma once

#include <stdint.h>
#include "../common/led_flag_common.h"

/* ESP32-C5: six independently sized outputs.  The number below is a safe
 * firmware ceiling; runtime config chooses the actual useful LEDs per output. */
#define LED_FLAG_UDP_PORT              7777
#define LED_FLAG_PACKET_COUNT          2 /* legacy/default frame: 1200 pixels */
#define LED_FLAG_MAX_PACKET_COUNT      ((LED_FLAG_TOTAL_LEDS + LED_FLAG_PIXELS_PER_PACKET - 1) / LED_FLAG_PIXELS_PER_PACKET)
#define LED_FLAG_PIXELS_PER_PACKET     600
#define LED_FLAG_PACKET_PAYLOAD_BYTES  1200
#define LED_FLAG_PROTOCOL_MAGIC        "C5P6"
#define LED_FLAG_ACK_MAGIC             "C5AK"
#define LED_FLAG_PROTOCOL_VERSION      2
#define LED_FLAG_FLAG_BRIGHTNESS_LIMIT  0x01U /* reserved = tope WS2815 0..255 */

/* XIAO ESP32-C5 serigrafia: D0, D1, D2, D4, D5, D6. */
#define LED_FLAG_GPIO_OUTPUT_1         1
#define LED_FLAG_GPIO_OUTPUT_2         0
#define LED_FLAG_GPIO_OUTPUT_3         25
#define LED_FLAG_GPIO_OUTPUT_4         23
#define LED_FLAG_GPIO_OUTPUT_5         24
#define LED_FLAG_GPIO_OUTPUT_6         11

#define LED_FLAG_SD_CS_GPIO            10
#define LED_FLAG_SD_SCLK_GPIO          4
#define LED_FLAG_SD_MISO_GPIO          5
#define LED_FLAG_SD_MOSI_GPIO          6
#define LED_FLAG_RESERVED_GPIO         7

#define LED_FLAG_MATRIX_ID_DEFAULT     1

typedef enum {
    LED_FLAG_PIXEL_FORMAT_RGB555_BE = 1,
} led_flag_pixel_format_t;

/* Header integers retain the existing C5 sender's little-endian layout.
 * Payload words are big-endian 0RRRRRGGGGGBBBBB. */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t pixel_format;
    uint8_t flags;    /* bit 0: C5 aplica el tope de brillo */
    uint8_t reserved; /* 0..255 cuando flags bit 0 = 1 */
    uint16_t frame_id;
    uint8_t packet_id;
    uint8_t packet_count;
    uint16_t pixel_offset;
    uint16_t pixel_count;
    uint16_t payload_size;
    uint16_t checksum;
} led_flag_packet_header_t;

#define LED_FLAG_MAX_PACKET_BYTES (sizeof(led_flag_packet_header_t) + LED_FLAG_PACKET_PAYLOAD_BYTES)

typedef enum {
    LED_FLAG_ACK_PACKET_RECEIVED = 1,
    LED_FLAG_ACK_FRAME_COMPLETE = 2,
    LED_FLAG_ACK_FRAME_DROPPED = 3,
} led_flag_ack_status_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t matrix_id;
    uint16_t frame_id;
    uint16_t packet_id;
    uint16_t packet_count;
    uint16_t received_packets;
    uint8_t status;
} led_flag_ack_t;

static inline uint16_t led_flag_checksum16(const uint8_t *data, uint16_t size)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < size; ++i) {
        sum += data[i];
    }
    return (uint16_t)sum;
}

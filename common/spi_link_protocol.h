#pragma once

#include <stdint.h>

#include "led_flag_common.h"

#define SPI_LINK_MAGIC                 "SPF1"
#define SPI_LINK_ACK_MAGIC             "SPA1"
#define SPI_LINK_SD_STATUS_MAGIC       "SDS1"
#define SPI_LINK_FRAME_CTRL_MAGIC      "C5C1"
#define SPI_LINK_UPLOAD_CTRL_MAGIC     "UPL1"
#define SPI_LINK_VERSION               1
#define SPI_LINK_MAX_PAYLOAD_BYTES     1024
#define SPI_LINK_SD_PATH_MAX           96
#define SPI_LINK_MAX_PACKET_BYTES      (sizeof(spi_link_packet_header_t) + SPI_LINK_MAX_PAYLOAD_BYTES)
#define SPI_LINK_FRAME_PACKET_COUNT    ((LED_FLAG_FRAME_SIZE_BYTES + SPI_LINK_MAX_PAYLOAD_BYTES - 1) / SPI_LINK_MAX_PAYLOAD_BYTES)
#define SPI_LINK_FLAG_LOOP             0x01
#define SPI_LINK_FLAG_RECORD_SHOW_LEDS 0x02
#define SPI_LINK_FLAG_CLOCK_SYNC       0x04

typedef enum {
    SPI_LINK_TYPE_FRAME_CHUNK = 1,
    SPI_LINK_TYPE_PING = 2,
    SPI_LINK_TYPE_COMMAND = 3,
    SPI_LINK_TYPE_OTA_CHUNK = 4,
    SPI_LINK_TYPE_SD_FILE_CHUNK = 5,
} spi_link_packet_type_t;

typedef enum {
    SPI_LINK_CMD_OTA_ENABLE = 1,
    SPI_LINK_CMD_OTA_DISABLE = 2,
    SPI_LINK_CMD_REBOOT = 3,
    SPI_LINK_CMD_PROXY_OTA_BEGIN = 10,
    SPI_LINK_CMD_PROXY_OTA_END = 11,
    SPI_LINK_CMD_PROXY_OTA_ABORT = 12,
    SPI_LINK_CMD_SD_RECORD_START = 20,
    SPI_LINK_CMD_SD_RECORD_STOP = 21,
    SPI_LINK_CMD_SD_PLAY_START = 22,
    SPI_LINK_CMD_SD_PLAY_STOP = 23,
    SPI_LINK_CMD_SD_STATUS = 24,
    SPI_LINK_CMD_SD_PLAY_NEXT_REC = 25,
    SPI_LINK_CMD_SD_DELETE_RECS = 26,
    SPI_LINK_CMD_SD_LIST_RECS = 27,
    SPI_LINK_CMD_SD_UPLOAD_BEGIN = 28,
    SPI_LINK_CMD_SD_UPLOAD_END = 29,
    SPI_LINK_CMD_SD_UPLOAD_ABORT = 30,
} spi_link_command_t;

typedef enum {
    SPI_LINK_STATUS_READY = 1,
    SPI_LINK_STATUS_BUSY = 2,
    SPI_LINK_STATUS_FRAME_COMPLETE = 3,
    SPI_LINK_STATUS_BAD_PACKET = 4,
    SPI_LINK_STATUS_COMMAND_OK = 5,
} spi_link_status_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t frame_id;
    uint16_t packet_id;
    uint16_t packet_count;
    uint32_t offset;
    uint16_t payload_size;
    uint16_t frame_size;
    uint16_t checksum;
} spi_link_packet_header_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t status;
    uint16_t last_frame_id;
    uint32_t received_frames;
    uint32_t dropped_frames;
} spi_link_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t arg0;
    uint16_t reserved;
    uint32_t value;
} spi_link_command_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t flags;
    uint16_t fps;
    uint32_t frames;
    char path[SPI_LINK_SD_PATH_MAX];
} spi_link_sd_control_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t reserved[3];
    uint32_t file_size;
    uint32_t checksum;
    char path[SPI_LINK_SD_PATH_MAX];
} spi_link_sd_upload_payload_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t command;
    uint8_t flags;
    uint16_t fps;
    uint32_t frames;
    char path[SPI_LINK_SD_PATH_MAX];
} spi_link_frame_control_payload_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t command;
    uint8_t reserved[3];
    uint32_t file_size;
    uint32_t checksum;
    char path[SPI_LINK_SD_PATH_MAX];
} spi_link_frame_upload_payload_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t status;
    uint16_t reserved;
    uint8_t sd_mounted;
    uint8_t recording;
    uint8_t playing;
    uint8_t reserved2;
    uint32_t recorded_frames;
    uint32_t record_dropped_frames;
    char file[SPI_LINK_SD_PATH_MAX];
} spi_link_sd_status_t;

static inline uint16_t spi_link_checksum16(const uint8_t *data, uint16_t size)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return (uint16_t)(sum & 0xffff);
}

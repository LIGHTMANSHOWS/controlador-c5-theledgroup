#pragma once

#include <stdint.h>

#define SCENE_MASTER_COMMAND_PORT 7780
#define SCENE_MASTER_STATUS_PORT  7781
#define SCENE_MASTER_VERSION      1
#define SCENE_MASTER_KEY          0x4c4d5033U
#define SCENE_MASTER_MAX_SCENES   10
#define SCENE_MASTER_NODE_COUNT   12

#define SCENE_MASTER_COMMAND_MAGIC "LSM1"
#define SCENE_MASTER_STATUS_MAGIC  "LSS1"

typedef enum {
    SCENE_MASTER_CMD_PLAY = 1,
    SCENE_MASTER_CMD_STOP = 2,
    SCENE_MASTER_CMD_LIVE = 3,
    SCENE_MASTER_CMD_PING = 4,
} scene_master_command_type_t;

typedef enum {
    SCENE_MASTER_NODE_IDLE = 0,
    SCENE_MASTER_NODE_PLAYING = 1,
    SCENE_MASTER_NODE_LIVE = 2,
    SCENE_MASTER_NODE_ERROR = 3,
} scene_master_node_state_t;

enum {
    SCENE_MASTER_FLAG_LOOP = 1U << 0,
    SCENE_MASTER_FLAG_SD_MOUNTED = 1U << 1,
    SCENE_MASTER_FLAG_SPI_OK = 1U << 2,
};

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t command;
    uint8_t scene;
    uint8_t flags;
    uint32_t command_id;
    uint16_t execute_delay_ms;
    uint16_t reserved;
    uint32_t key;
} scene_master_command_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t node_id;
    uint8_t state;
    uint8_t scene;
    uint8_t flags;
    uint8_t reserved[3];
    uint32_t command_id;
    uint32_t uptime_ms;
    uint32_t ipv4;
    uint32_t key;
} scene_master_status_t;


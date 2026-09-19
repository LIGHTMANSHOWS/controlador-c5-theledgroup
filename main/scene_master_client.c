#include "scene_master_client.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "rental_control.h"
#include "scene_master_protocol.h"
#include "spi_master_link.h"

static const char *TAG = "scene_client";

static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static scene_master_command_t s_pending;
static int64_t s_pending_due_us;
static bool s_pending_valid;
static uint32_t s_last_command_id;
static uint8_t s_state = SCENE_MASTER_NODE_IDLE;
static uint8_t s_scene;
static uint32_t s_status_command_id;

static bool command_valid(const scene_master_command_t *command)
{
    if (memcmp(command->magic, SCENE_MASTER_COMMAND_MAGIC, 4) != 0 ||
        command->version != SCENE_MASTER_VERSION ||
        command->key != SCENE_MASTER_KEY) {
        return false;
    }
    if (command->command < SCENE_MASTER_CMD_PLAY ||
        command->command > SCENE_MASTER_CMD_PING) {
        return false;
    }
    return command->command != SCENE_MASTER_CMD_PLAY ||
           (command->scene >= 1 && command->scene <= SCENE_MASTER_MAX_SCENES);
}

static void queue_command(const scene_master_command_t *command)
{
    taskENTER_CRITICAL(&s_pending_lock);
    if (command->command_id != s_last_command_id) {
        s_last_command_id = command->command_id;
        s_pending = *command;
        s_pending_due_us = esp_timer_get_time() + ((int64_t)command->execute_delay_ms * 1000);
        s_pending_valid = true;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
}

static void execute_command(const scene_master_command_t *command)
{
    esp_err_t ret = ESP_OK;
    switch (command->command) {
    case SCENE_MASTER_CMD_PLAY: {
        char path[32];
        snprintf(path, sizeof(path), "/sdcard/rec%u.lfs", command->scene);
        ret = rental_control_enable(false);
        if (ret == ESP_OK) {
            spi_master_link_set_frame_pause(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            ret = spi_master_link_send_sd_control(
                SPI_LINK_CMD_SD_PLAY_START,
                path,
                0,
                30,
                (command->flags & SCENE_MASTER_FLAG_LOOP) != 0,
                false,
                true);
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_RESPONSE) {
                spi_master_link_set_sd_activity(false, true);
                ret = ESP_OK;
            } else {
                spi_master_link_set_frame_pause(false);
            }
        }
        if (ret == ESP_OK) {
            s_state = SCENE_MASTER_NODE_PLAYING;
            s_scene = command->scene;
        }
        break;
    }
    case SCENE_MASTER_CMD_STOP:
        ret = rental_control_enable(false);
        s_state = SCENE_MASTER_NODE_IDLE;
        s_scene = 0;
        break;
    case SCENE_MASTER_CMD_LIVE:
        ret = rental_control_enable(false);
        s_state = SCENE_MASTER_NODE_LIVE;
        s_scene = 0;
        break;
    case SCENE_MASTER_CMD_PING:
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    s_status_command_id = command->command_id;
    if (ret != ESP_OK) {
        s_state = SCENE_MASTER_NODE_ERROR;
        ESP_LOGE(TAG,
                 "Comando maestro %u escena=%u fallo: %s",
                 command->command,
                 command->scene,
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG,
                 "Comando maestro ejecutado id=%" PRIu32 " cmd=%u escena=%u",
                 command->command_id,
                 command->command,
                 command->scene);
    }
}

static void executor_task(void *arg)
{
    (void)arg;
    while (true) {
        scene_master_command_t command = {0};
        bool execute = false;
        taskENTER_CRITICAL(&s_pending_lock);
        if (s_pending_valid && esp_timer_get_time() >= s_pending_due_us) {
            command = s_pending;
            s_pending_valid = false;
            execute = true;
        }
        taskEXIT_CRITICAL(&s_pending_lock);
        if (execute) {
            execute_command(&command);
        }
        vTaskDelay(1);
    }
}

static int open_command_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(SCENE_MASTER_COMMAND_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void command_receiver_task(void *arg)
{
    (void)arg;
    while (true) {
        int sock = open_command_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "No se pudo abrir UDP maestro: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Escuchando control maestro UDP %d", SCENE_MASTER_COMMAND_PORT);
        while (true) {
            scene_master_command_t command;
            int received = recv(sock, &command, sizeof(command), 0);
            if (received < 0) {
                ESP_LOGW(TAG, "recv control maestro fallo errno=%d", errno);
                break;
            }
            if (received == sizeof(command) && command_valid(&command)) {
                queue_command(&command);
            }
        }
        close(sock);
    }
}

static void status_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "No se pudo abrir socket de estado");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in master = {
        .sin_family = AF_INET,
        .sin_port = htons(SCENE_MASTER_STATUS_PORT),
    };
    inet_pton(AF_INET, "192.168.1.200", &master.sin_addr);

    while (true) {
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            esp_netif_get_ip_info(netif, &ip_info);
        }

        scene_master_status_t status = {
            .magic = SCENE_MASTER_STATUS_MAGIC,
            .version = SCENE_MASTER_VERSION,
            .node_id = CONFIG_LED_FLAG_MATRIX_ID,
            .state = s_state,
            .scene = s_scene,
            .flags = SCENE_MASTER_FLAG_SPI_OK,
            .command_id = s_status_command_id,
            .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .ipv4 = ip_info.ip.addr,
            .key = SCENE_MASTER_KEY,
        };
        sendto(sock, &status, sizeof(status), 0, (struct sockaddr *)&master, sizeof(master));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t scene_master_client_start(void)
{
    BaseType_t ok = xTaskCreate(command_receiver_task, "scene_rx", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreate(executor_task, "scene_exec", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreate(status_task, "scene_status", 4096, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

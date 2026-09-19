#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include "frame_buffer.h"
#include "fallback_image.h"
#include "led_flag_common.h"
#include "ota_http.h"
#include "rental_control.h"
#include "runtime_config.h"
#include "scene_master_client.h"
#include "spi_master_link.h"
#include "udp.h"
#include "wifi.h"

static const char *TAG = "main";

#if CONFIG_LED_FLAG_OFFLINE_ANIMATION
static void offline_fallback_task(void *arg)
{
    (void)arg;
    uint8_t *frame = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Sin memoria para imagen fija offline");
        vTaskDelete(NULL);
        return;
    }

    const TickType_t frame_period = pdMS_TO_TICKS(1000 / CONFIG_LED_FLAG_OFFLINE_FPS);
    TickType_t next_wake = xTaskGetTickCount();
    uint16_t frame_id = 60000;
    bool logged = false;

    while (true) {
        uint32_t idle_ms = udp_receiver_ms_since_last_packet();
        bool sd_active = spi_master_link_sd_recording() || spi_master_link_sd_playing();
        if (idle_ms >= CONFIG_LED_FLAG_OFFLINE_TIMEOUT_MS && !sd_active) {
            if (!logged) {
                ESP_LOGW(TAG, "Sin UDP por %" PRIu32 " ms: mostrando imagen fija C5", idle_ms);
                logged = true;
            }
            if (fallback_image_copy(frame, LED_FLAG_FRAME_SIZE_BYTES) != ESP_OK) {
                memset(frame, 0, LED_FLAG_FRAME_SIZE_BYTES);
            }
            esp_err_t ret = spi_master_link_queue_frame(frame_id++, frame);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo encolar frame offline: %s", esp_err_to_name(ret));
            }
            vTaskDelayUntil(&next_wake, frame_period);
        } else {
            if (logged) {
                ESP_LOGI(TAG,
                         "%s: saliendo de imagen fija offline",
                         sd_active ? "SD activa" : "UDP activo de nuevo");
            }
            logged = false;
            next_wake = xTaskGetTickCount() + frame_period;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Arrancando C5 integrado ESP32-C5: UDP+6 LED RGB555 (SD desactivada)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS inicializado");
    ESP_ERROR_CHECK(runtime_config_init());

    ret = frame_buffer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Inicializacion frame buffer fallida: %s", esp_err_to_name(ret));
        return;
    }

    ret = fallback_image_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Imagen fija C5 no iniciada: %s", esp_err_to_name(ret));
    }

    ret = spi_master_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backend local LED/SD no iniciado: %s", esp_err_to_name(ret));
        return;
    }
    spi_master_link_start_task();

    ret = rental_control_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Control alquiler no iniciado: %s", esp_err_to_name(ret));
    } else {
        rental_control_start_task();
    }

    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi no iniciado: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "El firmware queda vivo, pero no recibira UDP hasta configurar Wi-Fi.");
        return;
    }

    ret = ota_http_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA HTTP no iniciado: %s", esp_err_to_name(ret));
    }

    ret = udp_receiver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UDP no iniciado: %s", esp_err_to_name(ret));
        return;
    }

    udp_receiver_start_task();

    ret = scene_master_client_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cliente de escenas maestras no iniciado: %s", esp_err_to_name(ret));
    }

#if CONFIG_LED_FLAG_OFFLINE_ANIMATION
    xTaskCreate(offline_fallback_task, "offline_image", 4096, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "Listo: UDP RGB555 unicast, 2 paquetes/frame, 6x200 LED paralelos. SD desactivada.");
}

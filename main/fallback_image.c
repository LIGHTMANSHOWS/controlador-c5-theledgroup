#include "fallback_image.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "frame_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_flag_common.h"
#include "nvs.h"

static const char *TAG = "fallback_image";
static const char *NVS_NS = "fallback";
static const char *NVS_FRAME_KEY = "frame";
static const char *NVS_CHECKSUM_KEY = "checksum";

static uint8_t *s_frame;
static bool s_available;
static uint32_t s_checksum;
static SemaphoreHandle_t s_lock;

static uint32_t checksum32(const uint8_t *data, size_t size)
{
    uint32_t checksum = 2166136261U;
    for (size_t index = 0; index < size; index++) {
        checksum ^= data[index];
        checksum *= 16777619U;
    }
    return checksum;
}

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open lectura fallo");

    size_t size = LED_FLAG_FRAME_SIZE_BYTES;
    uint32_t stored_checksum = 0;
    ret = nvs_get_blob(nvs, NVS_FRAME_KEY, s_frame, &size);
    if (ret == ESP_OK) {
        ret = nvs_get_u32(nvs, NVS_CHECKSUM_KEY, &stored_checksum);
    }
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "leer respaldo NVS fallo");
    ESP_RETURN_ON_FALSE(size == LED_FLAG_FRAME_SIZE_BYTES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "respaldo NVS tiene tamano invalido");
    uint32_t actual = checksum32(s_frame, size);
    ESP_RETURN_ON_FALSE(actual == stored_checksum,
                        ESP_ERR_INVALID_CRC,
                        TAG,
                        "checksum de respaldo invalido");
    s_checksum = actual;
    s_available = true;
    return ESP_OK;
}

esp_err_t fallback_image_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "sin mutex");
    s_frame = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_frame != NULL, ESP_ERR_NO_MEM, TAG, "sin buffer");
    memset(s_frame, 0, LED_FLAG_FRAME_SIZE_BYTES);
    s_available = false;
    s_checksum = 0;

    esp_err_t ret = load_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Respaldo descartado: %s", esp_err_to_name(ret));
        s_available = false;
        s_checksum = 0;
        return ESP_OK;
    }
    ESP_LOGI(TAG,
             "Imagen fija C5: disponible=%u bytes=%u checksum=%08" PRIx32,
             s_available ? 1 : 0,
             s_available ? LED_FLAG_FRAME_SIZE_BYTES : 0,
             s_checksum);
    return ESP_OK;
}

esp_err_t fallback_image_capture_latest(void)
{
    uint8_t *snapshot = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_NO_MEM, TAG, "sin snapshot");
    esp_err_t ret = frame_buffer_copy_latest(snapshot, LED_FLAG_FRAME_SIZE_BYTES);
    if (ret != ESP_OK) {
        free(snapshot);
        return ret;
    }
    uint32_t checksum = checksum32(snapshot, LED_FLAG_FRAME_SIZE_BYTES);

    nvs_handle_t nvs = 0;
    ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(nvs, NVS_FRAME_KEY, snapshot, LED_FLAG_FRAME_SIZE_BYTES);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(nvs, NVS_CHECKSUM_KEY, checksum);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (ret == ESP_OK && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(s_frame, snapshot, LED_FLAG_FRAME_SIZE_BYTES);
        s_checksum = checksum;
        s_available = true;
        xSemaphoreGive(s_lock);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_TIMEOUT;
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    free(snapshot);
    ESP_RETURN_ON_ERROR(ret, TAG, "guardar imagen fija fallo");
    ESP_LOGI(TAG, "Imagen fija guardada: %u bytes checksum=%08" PRIx32,
             LED_FLAG_FRAME_SIZE_BYTES, checksum);
    return ESP_OK;
}

esp_err_t fallback_image_clear(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_erase_all(nvs);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "borrar imagen fija fallo");
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_frame, 0, LED_FLAG_FRAME_SIZE_BYTES);
        s_available = false;
        s_checksum = 0;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "Imagen fija C5 borrada");
    return ESP_OK;
}

esp_err_t fallback_image_copy(uint8_t *destination, size_t size)
{
    ESP_RETURN_ON_FALSE(destination != NULL, ESP_ERR_INVALID_ARG, TAG, "destino nulo");
    ESP_RETURN_ON_FALSE(size == LED_FLAG_FRAME_SIZE_BYTES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "tamano destino invalido");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "timeout mutex");
    if (!s_available) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(destination, s_frame, LED_FLAG_FRAME_SIZE_BYTES);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool fallback_image_is_available(void)
{
    bool available = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        available = s_available;
        xSemaphoreGive(s_lock);
    }
    return available;
}

uint32_t fallback_image_checksum(void)
{
    uint32_t checksum = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        checksum = s_checksum;
        xSemaphoreGive(s_lock);
    }
    return checksum;
}

void fallback_image_status_json(char *buffer, size_t size)
{
    bool available = fallback_image_is_available();
    snprintf(buffer,
             size,
             "\"fallback_available\":%s,\"fallback_bytes\":%u,\"fallback_checksum\":\"%08" PRIx32 "\"",
             available ? "true" : "false",
             available ? LED_FLAG_FRAME_SIZE_BYTES : 0,
             fallback_image_checksum());
}

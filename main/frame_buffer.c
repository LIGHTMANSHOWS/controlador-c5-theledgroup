#include "frame_buffer.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "protocol.h"
#include "spi_master_link.h"

static const char *TAG = "frame_buffer";
static uint8_t *s_receive_buffer;
static uint8_t *s_latest_buffer;
static bool s_has_complete_frame;
static SemaphoreHandle_t s_lock;

esp_err_t frame_buffer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "sin mutex");
    s_receive_buffer = heap_caps_calloc(1, LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    s_latest_buffer = heap_caps_calloc(1, LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_receive_buffer && s_latest_buffer, ESP_ERR_NO_MEM, TAG, "sin memoria");
    ESP_LOGI(TAG, "Doble buffer RGB555 listo: 2 x %u bytes", LED_FLAG_FRAME_SIZE_BYTES);
    return ESP_OK;
}

uint8_t *frame_buffer_get_receive_buffer(void)
{
    return s_receive_buffer;
}

esp_err_t frame_buffer_commit_receive_buffer(uint16_t frame_id, bool brightness_limit_enabled,
                                             uint8_t brightness_limit)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t *tmp = s_latest_buffer;
    s_latest_buffer = s_receive_buffer;
    s_receive_buffer = tmp;
    s_has_complete_frame = true;
    esp_err_t ret = spi_master_link_queue_frame_with_brightness(frame_id, s_latest_buffer,
                                                                 brightness_limit_enabled, brightness_limit);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t frame_buffer_copy_latest(uint8_t *destination, size_t size)
{
    ESP_RETURN_ON_FALSE(destination && size == LED_FLAG_FRAME_SIZE_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "destino invalido");
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_has_complete_frame) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(destination, s_latest_buffer, size);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool frame_buffer_has_complete_frame(void)
{
    return s_has_complete_frame;
}

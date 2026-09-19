#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol.h"

#define RUNTIME_CONFIG_COLOR_GRB 0
#define RUNTIME_CONFIG_COLOR_RGB 1
#define RUNTIME_CONFIG_COLOR_RBG 2
#define RUNTIME_CONFIG_COLOR_GBR 3
#define RUNTIME_CONFIG_COLOR_BRG 4
#define RUNTIME_CONFIG_COLOR_BGR 5

typedef struct {
    uint16_t pixels_per_output[LED_FLAG_OUTPUT_COUNT];
    uint8_t color_order;
    int8_t led_gpio[LED_FLAG_OUTPUT_COUNT];
    int8_t sd_cs_gpio;
    int8_t sd_sclk_gpio;
    int8_t sd_miso_gpio;
    int8_t sd_mosi_gpio;
    uint8_t record_to_memory_if_no_sd;
    uint8_t wifi_static_ip;
    char wifi_ssid[33];
    char wifi_password[65];
    char wifi_ip[16];
    char wifi_gateway[16];
    char wifi_netmask[16];
    /* Bit 0..5: mapper-enabled outputs. Disabled outputs retain their
     * configured lengths, but do not consume the fixed 1200-pixel frame. */
    uint32_t mapper_active_outputs_mask;
} runtime_config_t;

esp_err_t runtime_config_init(void);
const runtime_config_t *runtime_config_get(void);
esp_err_t runtime_config_save(const runtime_config_t *config);
bool runtime_config_pixels_valid(const runtime_config_t *config);
void runtime_config_json(char *buffer, size_t size);
const char *runtime_config_color_name(uint8_t order);
uint8_t runtime_config_color_from_name(const char *name, uint8_t fallback);

static inline bool runtime_config_output_active(const runtime_config_t *config, int output)
{
    return config != NULL && output >= 0 && output < LED_FLAG_OUTPUT_COUNT &&
           (config->mapper_active_outputs_mask & (1U << output)) != 0;
}

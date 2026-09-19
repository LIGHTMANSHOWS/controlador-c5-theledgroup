#include "runtime_config.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "runtime_cfg";
static const char *NVS_NS = "cfg";
static runtime_config_t s_config;

typedef struct {
    uint16_t pixels_per_output;
    uint8_t color_order;
    int8_t led_gpio[LED_FLAG_OUTPUT_COUNT];
    int8_t sd_cs_gpio;
    int8_t sd_sclk_gpio;
    int8_t sd_miso_gpio;
    int8_t sd_mosi_gpio;
    uint8_t record_to_memory_if_no_sd;
} runtime_config_v1_t;

typedef struct {
    uint16_t pixels_per_output[LED_FLAG_OUTPUT_COUNT];
    uint8_t color_order;
    int8_t led_gpio[LED_FLAG_OUTPUT_COUNT];
    int8_t sd_cs_gpio;
    int8_t sd_sclk_gpio;
    int8_t sd_miso_gpio;
    int8_t sd_mosi_gpio;
    uint8_t record_to_memory_if_no_sd;
} runtime_config_v2_t;

/* Current configuration before mapper-active outputs were introduced. */
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
} runtime_config_v3_t;

static void load_defaults(runtime_config_t *cfg)
{
    *cfg = (runtime_config_t) {
#if CONFIG_LED_FLAG_COLOR_ORDER_RGB
        .color_order = RUNTIME_CONFIG_COLOR_RGB,
#elif CONFIG_LED_FLAG_COLOR_ORDER_BRG
        .color_order = RUNTIME_CONFIG_COLOR_BRG,
#elif CONFIG_LED_FLAG_COLOR_ORDER_BGR
        .color_order = RUNTIME_CONFIG_COLOR_BGR,
#else
        .color_order = RUNTIME_CONFIG_COLOR_GRB,
#endif
        .led_gpio = {
            LED_FLAG_GPIO_OUTPUT_1,
            LED_FLAG_GPIO_OUTPUT_2,
            LED_FLAG_GPIO_OUTPUT_3,
            LED_FLAG_GPIO_OUTPUT_4,
            LED_FLAG_GPIO_OUTPUT_5,
            LED_FLAG_GPIO_OUTPUT_6,
        },
        .sd_cs_gpio = LED_FLAG_SD_CS_GPIO,
        .sd_sclk_gpio = LED_FLAG_SD_SCLK_GPIO,
        .sd_miso_gpio = LED_FLAG_SD_MISO_GPIO,
        .sd_mosi_gpio = LED_FLAG_SD_MOSI_GPIO,
        .record_to_memory_if_no_sd = 1,
#if CONFIG_LED_FLAG_WIFI_STATIC_IP
        .wifi_static_ip = 1,
#else
        .wifi_static_ip = 0,
#endif
    };
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        cfg->pixels_per_output[i] = 200;
    }
    cfg->mapper_active_outputs_mask = (1U << LED_FLAG_OUTPUT_COUNT) - 1U;
    strlcpy(cfg->wifi_ssid, CONFIG_LED_FLAG_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_password, CONFIG_LED_FLAG_WIFI_PASSWORD, sizeof(cfg->wifi_password));
    strlcpy(cfg->wifi_ip, CONFIG_LED_FLAG_WIFI_IP_ADDR, sizeof(cfg->wifi_ip));
    strlcpy(cfg->wifi_gateway, CONFIG_LED_FLAG_WIFI_GATEWAY, sizeof(cfg->wifi_gateway));
    strlcpy(cfg->wifi_netmask, CONFIG_LED_FLAG_WIFI_NETMASK, sizeof(cfg->wifi_netmask));
}

static void clamp_config(runtime_config_t *cfg)
{
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        if (cfg->pixels_per_output[i] > LED_FLAG_LEDS_PER_OUTPUT) {
            cfg->pixels_per_output[i] = LED_FLAG_LEDS_PER_OUTPUT;
        }
    }
    cfg->mapper_active_outputs_mask &= (1U << LED_FLAG_OUTPUT_COUNT) - 1U;
    if (cfg->color_order > RUNTIME_CONFIG_COLOR_BGR) {
        cfg->color_order = RUNTIME_CONFIG_COLOR_GRB;
    }
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->wifi_ip[sizeof(cfg->wifi_ip) - 1] = '\0';
    cfg->wifi_gateway[sizeof(cfg->wifi_gateway) - 1] = '\0';
    cfg->wifi_netmask[sizeof(cfg->wifi_netmask) - 1] = '\0';
    cfg->wifi_static_ip = cfg->wifi_static_ip ? 1 : 0;
}

bool runtime_config_pixels_valid(const runtime_config_t *cfg)
{
    if (cfg == NULL) return false;
    uint32_t total = 0;
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        if (cfg->pixels_per_output[i] > LED_FLAG_LEDS_PER_OUTPUT) return false;
        if (runtime_config_output_active(cfg, i)) total += cfg->pixels_per_output[i];
    }
    return total <= LED_FLAG_TOTAL_LEDS;
}

esp_err_t runtime_config_init(void)
{
    load_defaults(&s_config);
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Usando configuracion por defecto");
        return ESP_OK;
    }

    size_t size = sizeof(s_config);
    esp_err_t blob_ret = nvs_get_blob(nvs, "config", &s_config, &size);
    if (blob_ret != ESP_OK || size != sizeof(s_config)) {
        runtime_config_v3_t old3 = {0};
        size = sizeof(old3);
        if (nvs_get_blob(nvs, "config", &old3, &size) == ESP_OK && size == sizeof(old3)) {
            load_defaults(&s_config);
            memcpy(s_config.pixels_per_output, old3.pixels_per_output, sizeof(s_config.pixels_per_output));
            memcpy(s_config.led_gpio, old3.led_gpio, sizeof(s_config.led_gpio));
            s_config.color_order = old3.color_order;
            s_config.sd_cs_gpio = old3.sd_cs_gpio;
            s_config.sd_sclk_gpio = old3.sd_sclk_gpio;
            s_config.sd_miso_gpio = old3.sd_miso_gpio;
            s_config.sd_mosi_gpio = old3.sd_mosi_gpio;
            s_config.record_to_memory_if_no_sd = old3.record_to_memory_if_no_sd;
            s_config.wifi_static_ip = old3.wifi_static_ip;
            memcpy(s_config.wifi_ssid, old3.wifi_ssid, sizeof(s_config.wifi_ssid));
            memcpy(s_config.wifi_password, old3.wifi_password, sizeof(s_config.wifi_password));
            memcpy(s_config.wifi_ip, old3.wifi_ip, sizeof(s_config.wifi_ip));
            memcpy(s_config.wifi_gateway, old3.wifi_gateway, sizeof(s_config.wifi_gateway));
            memcpy(s_config.wifi_netmask, old3.wifi_netmask, sizeof(s_config.wifi_netmask));
        } else {
            runtime_config_v2_t old2 = {0};
            size = sizeof(old2);
            if (nvs_get_blob(nvs, "config", &old2, &size) == ESP_OK && size == sizeof(old2)) {
            load_defaults(&s_config);
            for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
                s_config.pixels_per_output[i] = old2.pixels_per_output[i];
                s_config.led_gpio[i] = old2.led_gpio[i];
            }
            s_config.color_order = old2.color_order;
            s_config.sd_cs_gpio = old2.sd_cs_gpio;
            s_config.sd_sclk_gpio = old2.sd_sclk_gpio;
            s_config.sd_miso_gpio = old2.sd_miso_gpio;
            s_config.sd_mosi_gpio = old2.sd_mosi_gpio;
            s_config.record_to_memory_if_no_sd = old2.record_to_memory_if_no_sd;
            } else {
                runtime_config_v1_t old = {0};
                size = sizeof(old);
                if (nvs_get_blob(nvs, "config", &old, &size) == ESP_OK && size == sizeof(old)) {
                    load_defaults(&s_config);
                    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
                        s_config.pixels_per_output[i] = old.pixels_per_output;
                        s_config.led_gpio[i] = old.led_gpio[i];
                    }
                    s_config.color_order = old.color_order;
                    s_config.sd_cs_gpio = old.sd_cs_gpio;
                    s_config.sd_sclk_gpio = old.sd_sclk_gpio;
                    s_config.sd_miso_gpio = old.sd_miso_gpio;
                    s_config.sd_mosi_gpio = old.sd_mosi_gpio;
                    s_config.record_to_memory_if_no_sd = old.record_to_memory_if_no_sd;
                } else {
                    load_defaults(&s_config);
                }
            }
        }
    }
    nvs_close(nvs);
    clamp_config(&s_config);
    ESP_LOGI(TAG,
             "Config activa: pixels=[%u,%u,%u,%u,%u,%u] active_mask=0x%02" PRIx32 " color=%s led=[%d,%d,%d,%d,%d,%d] sd=[%d,%d,%d,%d] mem_fallback=%u",
             s_config.pixels_per_output[0], s_config.pixels_per_output[1], s_config.pixels_per_output[2],
             s_config.pixels_per_output[3], s_config.pixels_per_output[4], s_config.pixels_per_output[5],
             s_config.mapper_active_outputs_mask,
             runtime_config_color_name(s_config.color_order),
             s_config.led_gpio[0], s_config.led_gpio[1], s_config.led_gpio[2],
             s_config.led_gpio[3], s_config.led_gpio[4], s_config.led_gpio[5],
             s_config.sd_cs_gpio, s_config.sd_sclk_gpio, s_config.sd_miso_gpio, s_config.sd_mosi_gpio,
             s_config.record_to_memory_if_no_sd);
    ESP_LOGI(TAG, "Wi-Fi config: ssid='%s' static=%u ip=%s gateway=%s netmask=%s",
             s_config.wifi_ssid,
             s_config.wifi_static_ip,
             s_config.wifi_ip,
             s_config.wifi_gateway,
             s_config.wifi_netmask);
    return ESP_OK;
}

const runtime_config_t *runtime_config_get(void)
{
    return &s_config;
}

esp_err_t runtime_config_save(const runtime_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config null");
    runtime_config_t copy = *config;
    clamp_config(&copy);
    ESP_RETURN_ON_FALSE(runtime_config_pixels_valid(&copy), ESP_ERR_INVALID_ARG, TAG,
                        "pixeles activos superan el cuadro de %u", LED_FLAG_TOTAL_LEDS);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t ret = nvs_set_blob(nvs, "config", &copy, sizeof(copy));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret == ESP_OK) {
        s_config = copy;
    }
    return ret;
}

const char *runtime_config_color_name(uint8_t order)
{
    switch (order) {
    case RUNTIME_CONFIG_COLOR_RGB: return "RGB";
    case RUNTIME_CONFIG_COLOR_RBG: return "RBG";
    case RUNTIME_CONFIG_COLOR_GBR: return "GBR";
    case RUNTIME_CONFIG_COLOR_BRG: return "BRG";
    case RUNTIME_CONFIG_COLOR_BGR: return "BGR";
    default: return "GRB";
    }
}

uint8_t runtime_config_color_from_name(const char *name, uint8_t fallback)
{
    if (name == NULL) return fallback;
    if (strcasecmp(name, "RGB") == 0) return RUNTIME_CONFIG_COLOR_RGB;
    if (strcasecmp(name, "RBG") == 0) return RUNTIME_CONFIG_COLOR_RBG;
    if (strcasecmp(name, "GBR") == 0) return RUNTIME_CONFIG_COLOR_GBR;
    if (strcasecmp(name, "BRG") == 0) return RUNTIME_CONFIG_COLOR_BRG;
    if (strcasecmp(name, "BGR") == 0) return RUNTIME_CONFIG_COLOR_BGR;
    if (strcasecmp(name, "GRB") == 0) return RUNTIME_CONFIG_COLOR_GRB;
    return fallback;
}

void runtime_config_json(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    snprintf(buffer,
             size,
             "\"config\":{\"outputs\":6,\"max_pixels_per_output\":%u,"
             "\"pixels_per_output\":[%u,%u,%u,%u,%u,%u],\"active_outputs_mask\":%" PRIu32 ",\"color_order\":\"%s\","
             "\"led_gpio\":[%d,%d,%d,%d,%d,%d],"
             "\"sd\":{\"cs\":%d,\"sclk\":%d,\"miso\":%d,\"mosi\":%d},"
             "\"wifi\":{\"ssid\":\"%s\",\"static_ip\":%s,\"ip\":\"%s\",\"gateway\":\"%s\",\"netmask\":\"%s\"},"
             "\"record_to_memory_if_no_sd\":%s}",
             LED_FLAG_LEDS_PER_OUTPUT,
             s_config.pixels_per_output[0], s_config.pixels_per_output[1],
             s_config.pixels_per_output[2], s_config.pixels_per_output[3],
             s_config.pixels_per_output[4], s_config.pixels_per_output[5],
             s_config.mapper_active_outputs_mask,
             runtime_config_color_name(s_config.color_order),
             s_config.led_gpio[0], s_config.led_gpio[1], s_config.led_gpio[2],
             s_config.led_gpio[3], s_config.led_gpio[4], s_config.led_gpio[5],
             s_config.sd_cs_gpio, s_config.sd_sclk_gpio, s_config.sd_miso_gpio, s_config.sd_mosi_gpio,
             s_config.wifi_ssid,
             s_config.wifi_static_ip ? "true" : "false",
             s_config.wifi_ip,
             s_config.wifi_gateway,
             s_config.wifi_netmask,
             s_config.record_to_memory_if_no_sd ? "true" : "false");
}

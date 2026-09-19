#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifndef WIFI_SSID
#define WIFI_SSID CONFIG_LED_FLAG_WIFI_SSID
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD CONFIG_LED_FLAG_WIFI_PASSWORD
#endif

esp_err_t wifi_init_sta(void);
bool wifi_is_connected(void);

typedef enum {
    WIFI_LINK_CONNECTING = 0,
    WIFI_LINK_CONNECTED,
    WIFI_LINK_RETRYING,
    WIFI_LINK_FAILED,
} wifi_link_state_t;

wifi_link_state_t wifi_link_state(void);
uint32_t wifi_retry_count(void);

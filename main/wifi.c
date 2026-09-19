#include "wifi.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

/*
 * ESP32-C5 is still a preview target in ESP-IDF 5.3.x. Some Wi-Fi Kconfig
 * symbols used by WIFI_INIT_CONFIG_DEFAULT() may be absent in that environment,
 * although newer C5-ready IDF versions define them normally.
 */
#ifndef CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 32
#endif
#ifndef CONFIG_ESP_WIFI_TX_BUFFER_TYPE
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF 0
#endif
#ifndef CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM 7
#endif

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "runtime_config.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_DISCONNECTED_BIT BIT2

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static esp_netif_t *s_sta_netif;
static int64_t s_wifi_start_us;
static int64_t s_connect_attempt_us;
static int64_t s_reconnect_due_us;
static volatile bool s_wifi_connected;
static volatile bool s_wifi_associated;
static volatile bool s_restart_wifi_before_reconnect;
static volatile wifi_link_state_t s_link_state = WIFI_LINK_CONNECTING;
static TaskHandle_t s_fast_connect_task_handle;

static int elapsed_ms_since(int64_t start_us)
{
    if (start_us == 0) {
        return 0;
    }
    return (int)((esp_timer_get_time() - start_us) / 1000);
}

static esp_err_t configure_static_ip(esp_netif_t *netif)
{
    const runtime_config_t *runtime = runtime_config_get();
    if (!runtime->wifi_static_ip) {
        ESP_LOGI(TAG, "Wi-Fi DHCP activado");
        return ESP_OK;
    }

    esp_netif_ip_info_t ip_info = { 0 };
    ip4_addr_t parsed_ip;
    ip4_addr_t parsed_gateway;
    ip4_addr_t parsed_netmask;

    if (ip4addr_aton(runtime->wifi_ip, &parsed_ip) == 0 ||
        ip4addr_aton(runtime->wifi_gateway, &parsed_gateway) == 0 ||
        ip4addr_aton(runtime->wifi_netmask, &parsed_netmask) == 0) {
        ESP_LOGE(TAG, "IP fija invalida: ip=%s gateway=%s netmask=%s",
                 runtime->wifi_ip,
                 runtime->wifi_gateway,
                 runtime->wifi_netmask);
        return ESP_ERR_INVALID_ARG;
    }
    ip_info.ip.addr = parsed_ip.addr;
    ip_info.gw.addr = parsed_gateway.addr;
    ip_info.netmask.addr = parsed_netmask.addr;

    esp_err_t dhcp_err = esp_netif_dhcpc_stop(netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "No se pudo detener DHCP para usar IP fija: %s", esp_err_to_name(dhcp_err));
        return dhcp_err;
    }
    esp_err_t err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar IP fija: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "IP fija configurada: " IPSTR " gateway " IPSTR " netmask " IPSTR,
             IP2STR(&ip_info.ip),
             IP2STR(&ip_info.gw),
             IP2STR(&ip_info.netmask));
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_link_state = WIFI_LINK_CONNECTING;
        ESP_LOGI(TAG, "Wi-Fi STA iniciado en %d ms", elapsed_ms_since(s_wifi_start_us));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_wifi_associated = true;
        s_link_state = WIFI_LINK_CONNECTING;
        ESP_LOGI(TAG,
                 "Wi-Fi asociado en %d ms desde connect, total boot Wi-Fi %d ms",
                 elapsed_ms_since(s_connect_attempt_us),
                 elapsed_ms_since(s_wifi_start_us));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG,
                 "Wi-Fi desconectado reason=%d intento=%d tiempo=%d ms. Supervisor reintentara...",
                 event ? event->reason : -1,
                 s_retry_num + 1,
                 elapsed_ms_since(s_connect_attempt_us));
        s_wifi_connected = false;
        s_wifi_associated = false;
        s_link_state = (s_retry_num >= 20) ? WIFI_LINK_FAILED : WIFI_LINK_RETRYING;
        s_connect_attempt_us = 0;
        s_restart_wifi_before_reconnect = (event != NULL);
        s_reconnect_due_us = esp_timer_get_time() + 80000;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;
        s_wifi_connected = true;
        s_link_state = WIFI_LINK_CONNECTED;
        ESP_LOGI(TAG,
                 "IP obtenida en %d ms desde connect, total boot Wi-Fi %d ms: " IPSTR,
                 elapsed_ms_since(s_connect_attempt_us),
                 elapsed_ms_since(s_wifi_start_us),
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_fast_connect_attempt(void)
{
    s_connect_attempt_us = esp_timer_get_time();
    s_wifi_associated = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
    s_link_state = s_retry_num == 0 ? WIFI_LINK_CONNECTING : WIFI_LINK_RETRYING;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect fallo: %s", esp_err_to_name(err));
    }
}

static void restart_wifi_radio_if_needed(void)
{
    if (!s_restart_wifi_before_reconnect) {
        return;
    }

    ESP_LOGW(TAG, "Reiniciando radio Wi-Fi antes de reintentar asociacion");
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop fallo durante recovery: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start fallo durante recovery: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    s_restart_wifi_before_reconnect = false;
}

static void fast_connect_supervisor_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_wifi_connected) {
            continue;
        }

        if (s_connect_attempt_us == 0) {
            if (s_reconnect_due_us != 0 && esp_timer_get_time() >= s_reconnect_due_us) {
                s_retry_num++;
                ESP_LOGW(TAG, "Reintentando Wi-Fi rapido intento=%d", s_retry_num + 1);
                s_reconnect_due_us = 0;
                restart_wifi_radio_if_needed();
                start_fast_connect_attempt();
            }
            continue;
        }

        int elapsed = elapsed_ms_since(s_connect_attempt_us);
        /*
         * El primer intento rapido normalmente conecta en ~1.2 s en REDPIXEL.
         * Si el AP devuelve un disconnect real, el event handler reintenta solo.
         * Evitamos cortar demasiado pronto un intento sin asociacion porque el
         * driver queda devolviendo reason=36 y ya no avanza hasta reiniciar.
         */
        int timeout_ms = s_wifi_associated ? (CONFIG_LED_FLAG_WIFI_CONNECT_RETRY_MS + 1200) :
                         (CONFIG_LED_FLAG_WIFI_CONNECT_RETRY_MS < 3500 ? 3500 :
                          CONFIG_LED_FLAG_WIFI_CONNECT_RETRY_MS);
        if (elapsed <= timeout_ms) {
            continue;
        }

        s_retry_num++;
        ESP_LOGW(TAG,
                 "Intento Wi-Fi %d excedio %d ms (%d ms reales, asociado=%d). Cortando y reintentando directo...",
                 s_retry_num,
                 timeout_ms,
                 elapsed,
                 s_wifi_associated);

        s_connect_attempt_us = 0;
        s_reconnect_due_us = 0;
        s_restart_wifi_before_reconnect = true;
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_DISCONNECTED_BIT,
                            pdTRUE,
                            pdFALSE,
                            pdMS_TO_TICKS(400));
        vTaskDelay(pdMS_TO_TICKS(80));
        restart_wifi_radio_if_needed();
        start_fast_connect_attempt();
    }
}

static void apply_fast_wifi_runtime_options(void)
{
#if CONFIG_LED_FLAG_WIFI_FAST_CONNECT
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
#endif
}

static bool parse_bssid(const char *text, uint8_t bssid[6])
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    unsigned int mac[6] = { 0 };
    int parsed = sscanf(text,
                        "%02x:%02x:%02x:%02x:%02x:%02x",
                        &mac[0],
                        &mac[1],
                        &mac[2],
                        &mac[3],
                        &mac[4],
                        &mac[5]);
    if (parsed != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        if (mac[i] > 0xff) {
            return false;
        }
        bssid[i] = (uint8_t)mac[i];
    }
    return true;
}

esp_err_t wifi_init_sta(void)
{
    const runtime_config_t *runtime = runtime_config_get();
    if (strlen(runtime->wifi_ssid) == 0) {
        ESP_LOGE(TAG, "SSID vacio. Configuralo desde /config o menuconfig.");
        s_link_state = WIFI_LINK_FAILED;
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el EventGroup de Wi-Fi");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(configure_static_ip(s_sta_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    apply_fast_wifi_runtime_options();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, runtime->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, runtime->wifi_password, sizeof(wifi_config.sta.password));

#if CONFIG_LED_FLAG_WIFI_FAST_CONNECT
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.channel = CONFIG_LED_FLAG_WIFI_CHANNEL;
    wifi_config.sta.failure_retry_cnt = 1;
#else
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
#endif

    wifi_config.sta.threshold.rssi = CONFIG_LED_FLAG_WIFI_MIN_RSSI;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    wifi_config.sta.sae_pk_mode = WPA3_SAE_PK_MODE_DISABLED;

#if CONFIG_LED_FLAG_WIFI_FAST_CONNECT
    uint8_t preferred_bssid[6] = { 0 };
    if (parse_bssid(CONFIG_LED_FLAG_WIFI_BSSID, preferred_bssid)) {
        wifi_config.sta.bssid_set = true;
        memcpy(wifi_config.sta.bssid, preferred_bssid, sizeof(preferred_bssid));
        ESP_LOGI(TAG,
                 "BSSID Wi-Fi fijado en %02x:%02x:%02x:%02x:%02x:%02x",
                 preferred_bssid[0],
                 preferred_bssid[1],
                 preferred_bssid[2],
                 preferred_bssid[3],
                 preferred_bssid[4],
                 preferred_bssid[5]);
    } else if (strlen(CONFIG_LED_FLAG_WIFI_BSSID) > 0) {
        ESP_LOGW(TAG, "BSSID Wi-Fi invalido: '%s'. Se usara solo SSID/canal.", CONFIG_LED_FLAG_WIFI_BSSID);
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_wifi_start_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_LED_FLAG_WIFI_FAST_CONNECT && CONFIG_LED_FLAG_WIFI_5G_ONLY && CONFIG_SOC_WIFI_SUPPORT_5G
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (band_err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi fijado en banda 5 GHz");
    } else {
        ESP_LOGW(TAG, "No se pudo fijar Wi-Fi 5 GHz: %s", esp_err_to_name(band_err));
    }
#endif
#if 0
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC,
    };
    esp_err_t proto_err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols);
    if (proto_err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi protocolos fijados: 2.4=b/g/n, 5=a/n/ac (AX off)");
    } else {
        ESP_LOGW(TAG, "No se pudieron fijar protocolos Wi-Fi: %s", esp_err_to_name(proto_err));
    }
#endif

    if (s_fast_connect_task_handle == NULL) {
        xTaskCreate(fast_connect_supervisor_task,
                    "wifi_fast_retry",
                    3072,
                    NULL,
                    8,
                    &s_fast_connect_task_handle);
    }

    start_fast_connect_attempt();

    ESP_LOGI(TAG,
             "Wi-Fi rapido directo SSID='%s' canal=%d bssid='%s' retry=%d ms min_rssi=%d. Power-save OFF.",
             runtime->wifi_ssid,
             CONFIG_LED_FLAG_WIFI_CHANNEL,
             CONFIG_LED_FLAG_WIFI_BSSID,
             CONFIG_LED_FLAG_WIFI_CONNECT_RETRY_MS,
             CONFIG_LED_FLAG_WIFI_MIN_RSSI
    );
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

wifi_link_state_t wifi_link_state(void)
{
    return s_link_state;
}

uint32_t wifi_retry_count(void)
{
    return (uint32_t)s_retry_num;
}

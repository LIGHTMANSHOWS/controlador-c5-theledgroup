#include "ota_http.h"

#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fallback_image.h"
#include "rental_control.h"
#include "runtime_config.h"
#include "spi_link_protocol.h"
#include "spi_master_link.h"
#include "udp.h"
#include "leds.h"

static const char *TAG = "ota_http";
static bool s_sd_proxy_mounted;
static bool s_sd_proxy_recording;
static bool s_sd_proxy_playing;
static uint32_t s_sd_proxy_recorded_frames;
static uint32_t s_sd_proxy_dropped_frames;
static char s_sd_proxy_file[SPI_LINK_SD_PATH_MAX] = "/sdcard/test.lfs";
static TaskHandle_t s_test_task_handle;
static volatile uint8_t s_test_mode;

enum {
    TEST_MODE_OFF = 0,
    TEST_MODE_OUTPUTS,
    TEST_MODE_WHITE,
    TEST_MODE_RED,
    TEST_MODE_GREEN,
    TEST_MODE_BLUE,
    TEST_MODE_GRAY_RAMP,
    TEST_MODE_SCAN,
};

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<title>C5 ESP32-C5 6 salidas variables</title></head><body>"
    "<h2>C5 integrado: UDP + 6 salidas LED variables</h2>"
    "<p><a href='/status'>status</a> | <a href='/config'>config</a> | <a href='/test'>tester</a></p>"
    "<p>Imagen fija C5: <a href='/fallback/capture'>guardar cuadro actual</a> | "
    "<a href='/fallback/status'>estado</a> | <a href='/fallback/clear'>borrar</a></p>"
    "<form method='POST' action='/ota' enctype='application/octet-stream'>"
    "<input type='file' name='firmware'>"
    "<button type='submit'>Subir firmware .bin</button>"
    "</form>"
    "<p>curl -X POST --data-binary @firmware.bin http://IP/ota</p>"
    "</body></html>";

static void get_query_value(httpd_req_t *req, const char *key, char *value, size_t value_size);

static uint16_t test_rgb555(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 10) | ((uint16_t)(g >> 3) << 5) | (b >> 3);
}

static void test_set_pixel(uint8_t *frame, int index, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t word = test_rgb555(r, g, b);
    frame[index * 2] = (uint8_t)(word >> 8);
    frame[index * 2 + 1] = (uint8_t)word;
}

static const char *test_mode_name(uint8_t mode)
{
    switch (mode) {
    case TEST_MODE_OUTPUTS: return "outputs";
    case TEST_MODE_WHITE: return "white";
    case TEST_MODE_RED: return "red";
    case TEST_MODE_GREEN: return "green";
    case TEST_MODE_BLUE: return "blue";
    case TEST_MODE_GRAY_RAMP: return "gray_ramp";
    case TEST_MODE_SCAN: return "scan";
    default: return "off";
    }
}

static uint8_t test_mode_from_name(const char *name)
{
    if (name == NULL || strcmp(name, "off") == 0) return TEST_MODE_OFF;
    if (strcmp(name, "outputs") == 0) return TEST_MODE_OUTPUTS;
    if (strcmp(name, "white") == 0) return TEST_MODE_WHITE;
    if (strcmp(name, "red") == 0) return TEST_MODE_RED;
    if (strcmp(name, "green") == 0) return TEST_MODE_GREEN;
    if (strcmp(name, "blue") == 0) return TEST_MODE_BLUE;
    if (strcmp(name, "gray_ramp") == 0) return TEST_MODE_GRAY_RAMP;
    if (strcmp(name, "scan") == 0) return TEST_MODE_SCAN;
    return TEST_MODE_OFF;
}

static void test_fill_frame(uint8_t *frame, uint8_t mode, uint32_t tick)
{
    memset(frame, 0, LED_FLAG_FRAME_SIZE_BYTES);
    const runtime_config_t *cfg = runtime_config_get();
    int frame_pixel = 0;
    for (int output = 0; output < LED_FLAG_OUTPUT_COUNT; ++output) {
        int count = cfg->pixels_per_output[output];
        for (int pos = 0; pos < count; ++pos, ++frame_pixel) {
        uint8_t r = 0, g = 0, b = 0;
        switch (mode) {
        case TEST_MODE_OUTPUTS: {
            static const uint8_t colors[6][3] = {
                {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
            };
            r = colors[output][0]; g = colors[output][1]; b = colors[output][2];
            break;
        }
        case TEST_MODE_WHITE: r = 255; g = 255; b = 255; break;
        case TEST_MODE_RED: r = 255; break;
        case TEST_MODE_GREEN: g = 255; break;
        case TEST_MODE_BLUE: b = 255; break;
        case TEST_MODE_GRAY_RAMP:
            r = g = b = count < 2 ? 255 : (uint8_t)((pos * 255) / (count - 1));
            break;
        case TEST_MODE_SCAN:
            if (count > 0 && pos == (int)(tick % count)) r = g = b = 255;
            break;
        default:
            break;
        }
        test_set_pixel(frame, frame_pixel, r, g, b);
        }
    }
}

static void test_task(void *arg)
{
    (void)arg;
    uint8_t *frame = malloc(LED_FLAG_FRAME_SIZE_BYTES);
    if (!frame) {
        ESP_LOGE(TAG, "Sin memoria para tester");
        vTaskDelete(NULL);
        return;
    }
    uint32_t tick = 0;
    while (true) {
        uint8_t mode = s_test_mode;
        if (mode == TEST_MODE_OFF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        test_fill_frame(frame, mode, tick++);
        (void)leds_commit_frame(frame);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static void html_input(char *dst, size_t size, const char *name, int value)
{
    size_t used = strlen(dst);
    snprintf(dst + used, used < size ? size - used : 0,
             "<label>%s <input name='%s' type='number' value='%d'></label><br>",
             name, name, value);
}

static void html_text_input(char *dst, size_t size, const char *label, const char *name, const char *value, const char *type)
{
    size_t used = strlen(dst);
    snprintf(dst + used, used < size ? size - used : 0,
             "<label>%s <input name='%s' type='%s' value='%s'></label><br>",
             label, name, type, value != NULL ? value : "");
}

static int xiao_d_to_gpio(int d_pin)
{
    static const int gpio_by_d[] = {1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10};
    if (d_pin < 0 || d_pin >= (int)(sizeof(gpio_by_d) / sizeof(gpio_by_d[0]))) {
        return -1;
    }
    return gpio_by_d[d_pin];
}

static int xiao_gpio_to_d(int gpio)
{
    for (int d = 0; d <= 10; ++d) {
        if (xiao_d_to_gpio(d) == gpio) {
            return d;
        }
    }
    return -1;
}

static void html_xiao_d_select(char *dst, size_t size, const char *label, const char *name, int gpio)
{
    int selected_d = xiao_gpio_to_d(gpio);
    size_t used = strlen(dst);
    snprintf(dst + used, used < size ? size - used : 0,
             "<label>%s <select name='%s'>", label, name);
    for (int d = 0; d <= 10; ++d) {
        used = strlen(dst);
        snprintf(dst + used, used < size ? size - used : 0,
                 "<option value='D%d' %s>D%d</option>",
                 d,
                 d == selected_d ? "selected" : "",
                 d);
    }
    strlcat(dst, "</select></label><br>", size);
}

static int get_query_xiao_gpio(httpd_req_t *req, const char *key, int fallback_gpio)
{
    char value[8];
    get_query_value(req, key, value, sizeof(value));
    if (value[0] == 'D' || value[0] == 'd') {
        int gpio = xiao_d_to_gpio(atoi(value + 1));
        return gpio >= 0 ? gpio : fallback_gpio;
    }
    if (value[0] != '\0') {
        int gpio = xiao_d_to_gpio(atoi(value));
        return gpio >= 0 ? gpio : fallback_gpio;
    }
    return fallback_gpio;
}

static esp_err_t config_json_get_handler(httpd_req_t *req)
{
    char cfg[900];
    runtime_config_json(cfg, sizeof(cfg));
    char response[960];
    snprintf(response, sizeof(response), "{%s,\"needs_reboot_after_save\":true}\n", cfg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static void html_test_option(char *dst, size_t size, const char *value, const char *label, uint8_t selected)
{
    size_t used = strlen(dst);
    snprintf(dst + used, used < size ? size - used : 0,
             "<option value='%s' %s>%s</option>",
             value,
             strcmp(test_mode_name(selected), value) == 0 ? "selected" : "",
             label);
}

static esp_err_t test_get_handler(httpd_req_t *req)
{
    char mode[24];
    get_query_value(req, "mode", mode, sizeof(mode));
    if (mode[0] != '\0') {
        uint8_t new_mode = test_mode_from_name(mode);
        s_test_mode = new_mode;
        spi_master_link_set_frame_pause(new_mode != TEST_MODE_OFF);
    }

    uint8_t current = s_test_mode;
    char html[1800];
    snprintf(html, sizeof(html),
             "<!doctype html><html><head><meta charset='utf-8'><title>C5 tester</title></head><body>"
             "<h2>C5 tester de pixeles</h2>"
             "<p>Modo actual: <b>%s</b></p>"
             "<p>Cuando el tester esta activo, se pausa LIVE UDP. Pon <b>off</b> para volver a recibir Art-Net/live.</p>"
             "<form method='GET' action='/test'>"
             "<label>modo <select name='mode'>",
             test_mode_name(current));
    html_test_option(html, sizeof(html), "off", "off - live normal", current);
    html_test_option(html, sizeof(html), "outputs", "outputs - cada salida color distinto", current);
    html_test_option(html, sizeof(html), "white", "white - blanco total", current);
    html_test_option(html, sizeof(html), "red", "red - rojo", current);
    html_test_option(html, sizeof(html), "green", "green - verde", current);
    html_test_option(html, sizeof(html), "blue", "blue - azul", current);
    html_test_option(html, sizeof(html), "gray_ramp", "gray ramp - brillo por pixel", current);
    html_test_option(html, sizeof(html), "scan", "scan - pixel moviendose", current);
    strlcat(html,
            "</select></label> <button type='submit'>Aplicar</button>"
            "</form>"
            "<p>Nota: el primer LED fisico de cada salida sigue reservado para estado/nivelador.</p>"
            "<p><a href='/status'>status</a> | <a href='/'>inicio</a></p>"
            "</body></html>",
            sizeof(html));
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    const runtime_config_t *cfg = runtime_config_get();
    char html[5200];
    snprintf(html,
             sizeof(html),
             "<!doctype html><html><head><meta charset='utf-8'><title>C5 config</title></head><body>"
             "<h2>C5 Config</h2>"
             "<form method='GET' action='/config/save'>"
             "<p><button type='submit'>Save / Guardar</button></p>"
             "<h3>LED</h3>"
             "<label>orden color <select name='order'>"
             "<option %s>GRB</option><option %s>RGB</option><option %s>RBG</option>"
             "<option %s>GBR</option><option %s>BRG</option><option %s>BGR</option>"
             "</select></label><br>",
             cfg->color_order == RUNTIME_CONFIG_COLOR_GRB ? "selected" : "",
             cfg->color_order == RUNTIME_CONFIG_COLOR_RGB ? "selected" : "",
             cfg->color_order == RUNTIME_CONFIG_COLOR_RBG ? "selected" : "",
             cfg->color_order == RUNTIME_CONFIG_COLOR_GBR ? "selected" : "",
             cfg->color_order == RUNTIME_CONFIG_COLOR_BRG ? "selected" : "",
             cfg->color_order == RUNTIME_CONFIG_COLOR_BGR ? "selected" : "");
    strlcat(html, "<h3>Pixeles por salida</h3>"
             "<p>Pixeles utiles de show. El primer LED fisico de cada salida queda reservado para nivel/estado.</p>",
             sizeof(html));
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        char name[8];
        snprintf(name, sizeof(name), "pix%d", i + 1);
        html_input(html, sizeof(html), name, cfg->pixels_per_output[i]);
    }
    strlcat(html, "<h3>Pines LED</h3>", sizeof(html));
    strlcat(html, "<p>XIAO ESP32-C5: selecciona la serigrafia D impresa en la placa.</p>", sizeof(html));
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        char label[8];
        char name[10];
        snprintf(label, sizeof(label), "led%d", i + 1);
        snprintf(name, sizeof(name), "led%dd", i + 1);
        html_xiao_d_select(html, sizeof(html), label, name, cfg->led_gpio[i]);
    }
    strlcat(html, "<h3>Wi-Fi / Red</h3>", sizeof(html));
    html_text_input(html, sizeof(html), "SSID", "ssid", cfg->wifi_ssid, "text");
    html_text_input(html, sizeof(html), "password", "pass", cfg->wifi_password, "password");
    strlcat(html, "<label><input name='staticip' type='checkbox' value='1' ", sizeof(html));
    strlcat(html, cfg->wifi_static_ip ? "checked" : "", sizeof(html));
    strlcat(html, "> usar IP fija</label><br>", sizeof(html));
    html_text_input(html, sizeof(html), "ip", "ip", cfg->wifi_ip, "text");
    html_text_input(html, sizeof(html), "gateway", "gateway", cfg->wifi_gateway, "text");
    html_text_input(html, sizeof(html), "netmask", "netmask", cfg->wifi_netmask, "text");
    strlcat(html,
            "<p>SD desactivada: los pines D quedan libres para LED.</p><br>"
            "<button type='submit'>Save / Guardar</button>"
            "</form><p>Despues de guardar, reinicia el C5 para aplicar pines y red Wi-Fi.</p>"
            "<p><a href='/config.json'>config.json</a> | <a href='/reboot'>reiniciar</a> | <a href='/'>inicio</a></p>"
            "</body></html>",
            sizeof(html));
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char response[2400];
    char rental_json[160];
    char fallback_json[180];
    rental_control_status_json(rental_json, sizeof(rental_json));
    fallback_image_status_json(fallback_json, sizeof(fallback_json));
    const char *sd_json = "\"sd\":{\"sd_mounted\":false,\"recording\":false,\"playing\":false,\"sd_queue_depth\":0,\"recorded_frames\":0,\"record_dropped_frames\":0,\"file\":\"\",\"sd_last_error\":\"disabled\"}";
    char cfg_json[900];
    runtime_config_json(cfg_json, sizeof(cfg_json));
    wifi_ap_record_t ap = {0};
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
    uint32_t idle_ms = udp_receiver_ms_since_last_packet();
    int len = snprintf(response, sizeof(response),
                       "{"
                       "\"ok\":true,"
                       "\"project\":\"%s\","
                       "\"version\":\"%s\","
                       "\"idf\":\"%s\","
                       "\"partition\":\"%s\","
                       "\"mode\":\"live_record_playback_local\","
                       "\"pixel_format\":\"RGB555_BE\","
                       "\"outputs\":6,\"max_pixels_per_output\":%u,"
                       "\"free_heap\":%" PRIu32 ","
                       "\"uptime_ms\":%" PRIu32 ","
                       "\"matrix_id\":%u,"
                       "\"udp_idle_ms\":%" PRIu32 ","
                       "\"udp_valid_packets\":%" PRIu32 ","
                       "\"udp_invalid_packets\":%" PRIu32 ","
                       "\"udp_completed_frames\":%" PRIu32 ","
                       "\"rx_fps\":%" PRIu32 ",\"led_fps\":%" PRIu32 ","
                       "\"frames_incomplete\":%" PRIu32 ",\"frames_discarded\":%" PRIu32 ","
                       "\"packets_lost\":%" PRIu32 ",\"last_frame_id\":%u,"
                       "\"assembly_us\":%" PRIu32 ",\"led_output_us\":%" PRIu32 ","
                       "\"led_frames\":%" PRIu32 ",\"led_dropped_frames\":%" PRIu32 ","
                       "\"brightness_limit_protocol\":true,\"brightness_limit_active\":%s,\"brightness_limit_value\":%u,"
                       "\"live_paused\":%s,\"wifi_rssi\":%d,"
                       "%s,"
                       "%s,"
                       "%s,"
                       "%s"
                       "}\n",
                       app->project_name,
                       app->version,
                       app->idf_ver,
                       running ? running->label : "unknown",
                       LED_FLAG_LEDS_PER_OUTPUT,
                       (uint32_t)esp_get_free_heap_size(),
                       (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                       CONFIG_LED_FLAG_MATRIX_ID,
                       idle_ms,
                       udp_receiver_valid_packets(),
                       udp_receiver_invalid_packets(),
                       udp_receiver_completed_frames(),
                       udp_receiver_rx_fps(),
                       leds_output_fps(),
                       udp_receiver_incomplete_frames(),
                       udp_receiver_discarded_frames(),
                       udp_receiver_detected_lost_packets(),
                       udp_receiver_last_frame_id(),
                       udp_receiver_last_assembly_us(),
                       leds_last_output_us(),
                       spi_master_link_sent_frames(),
                       spi_master_link_dropped_frames(),
                       leds_brightness_limit_active() ? "true" : "false",
                       leds_brightness_limit_value(),
                       spi_master_link_frames_paused() ? "true" : "false",
                       rssi,
                       sd_json,
                       cfg_json,
                       rental_json,
                       fallback_json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, len);
}

static esp_err_t fallback_capture_get_handler(httpd_req_t *req)
{
    esp_err_t ret = fallback_image_capture_latest();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req,
                            ret == ESP_ERR_INVALID_STATE ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(ret));
        return ret;
    }
    rental_control_enable(false);
    char status[180];
    char response[220];
    fallback_image_status_json(status, sizeof(status));
    snprintf(response, sizeof(response), "{\"ok\":true,%s}\n", status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t fallback_clear_get_handler(httpd_req_t *req)
{
    rental_control_enable(false);
    esp_err_t ret = fallback_image_clear();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"fallback_available\":false}\n");
}

static esp_err_t fallback_status_get_handler(httpd_req_t *req)
{
    char status[180];
    char response[220];
    fallback_image_status_json(status, sizeof(status));
    snprintf(response, sizeof(response), "{%s}\n", status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t send_cam_command(httpd_req_t *req, spi_link_command_t command, const char *message)
{
    esp_err_t ret = ESP_FAIL;
    bool transmitted_once = false;
    bool ack_once = false;

    spi_master_link_set_frame_pause(true);
    vTaskDelay(pdMS_TO_TICKS(80));

    for (int attempt = 0; attempt < 12; attempt++) {
        ret = spi_master_link_send_command((uint8_t)command, 0);
        if (ret == ESP_OK) {
            ack_once = true;
            transmitted_once = true;
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            /*
             * El comando ya salio por MOSI pero el ACK por MISO puede perderse
             * si el slave justo estaba ocupando el RMT/flash. Para ON/OFF/REBOOT
             * nos interesa repetir el comando, no bloquear al usuario por el ACK.
             */
            transmitted_once = true;
        } else {
            ESP_LOGW(TAG,
                     "Comando CAM %u intento %d fallo: %s",
                     command,
                     attempt + 1,
                     esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    spi_master_link_set_frame_pause(false);

    if (!transmitted_once) {
        ESP_LOGW(TAG, "No se pudo enviar comando CAM %u: %s", command, esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_set_type(req, "text/plain");
    ESP_LOGI(TAG, "Comando CAM %u enviado. ack_valido=%s", command, ack_once ? "si" : "no");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t cam_ota_on_get_handler(httpd_req_t *req)
{
    return send_cam_command(req,
                            SPI_LINK_CMD_OTA_ENABLE,
                            "Comando enviado: ESPCAM Wi-Fi/OTA ON. Espera 5-10 s y abre http://192.168.1.207/status\n");
}

static esp_err_t cam_ota_off_get_handler(httpd_req_t *req)
{
    return send_cam_command(req,
                            SPI_LINK_CMD_OTA_DISABLE,
                            "Comando enviado: ESPCAM Wi-Fi/OTA OFF.\n");
}

static esp_err_t cam_reboot_get_handler(httpd_req_t *req)
{
    return send_cam_command(req, SPI_LINK_CMD_REBOOT, "Comando enviado: ESPCAM reboot.\n");
}

static void get_query_value(httpd_req_t *req, const char *key, char *value, size_t value_size)
{
    char query[1400];
    if (value == NULL || value_size == 0) {
        return;
    }
    value[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, key, value, value_size);
    }

    char *read = value;
    char *write = value;
    while (*read != '\0') {
        if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            char hex[3] = { read[1], read[2], '\0' };
            *write++ = (char)strtoul(hex, NULL, 16);
            read += 3;
        } else if (*read == '+') {
            *write++ = ' ';
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static int get_query_int(httpd_req_t *req, const char *key, int fallback)
{
    char value[24];
    get_query_value(req, key, value, sizeof(value));
    return value[0] != '\0' ? atoi(value) : fallback;
}

static esp_err_t config_save_get_handler(httpd_req_t *req)
{
    runtime_config_t cfg = *runtime_config_get();
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "pix%d", i + 1);
        cfg.pixels_per_output[i] = (uint16_t)get_query_int(req, key, cfg.pixels_per_output[i]);
    }
    char order[8];
    get_query_value(req, "order", order, sizeof(order));
    cfg.color_order = runtime_config_color_from_name(order, cfg.color_order);
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "led%dd", i + 1);
        cfg.led_gpio[i] = (int8_t)get_query_xiao_gpio(req, key, cfg.led_gpio[i]);
    }
    char text[80];
    get_query_value(req, "ssid", text, sizeof(text));
    if (text[0] != '\0') {
        strlcpy(cfg.wifi_ssid, text, sizeof(cfg.wifi_ssid));
    }
    get_query_value(req, "pass", text, sizeof(text));
    if (text[0] != '\0') {
        strlcpy(cfg.wifi_password, text, sizeof(cfg.wifi_password));
    }
    get_query_value(req, "ip", text, sizeof(text));
    if (text[0] != '\0') {
        strlcpy(cfg.wifi_ip, text, sizeof(cfg.wifi_ip));
    }
    get_query_value(req, "gateway", text, sizeof(text));
    if (text[0] != '\0') {
        strlcpy(cfg.wifi_gateway, text, sizeof(cfg.wifi_gateway));
    }
    get_query_value(req, "netmask", text, sizeof(text));
    if (text[0] != '\0') {
        strlcpy(cfg.wifi_netmask, text, sizeof(cfg.wifi_netmask));
    }
    char staticip[8];
    get_query_value(req, "staticip", staticip, sizeof(staticip));
    cfg.wifi_static_ip = staticip[0] != '\0';
    cfg.record_to_memory_if_no_sd = 1;
    esp_err_t ret = runtime_config_save(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req,
                              "<!doctype html><html><body><h2>Config guardada</h2>"
                              "<p>Reinicia el C5 para aplicar cambios de pines y red Wi-Fi.</p>"
                              "<p><a href='/reboot'>Reiniciar ahora</a> | <a href='/config'>Volver</a></p>"
                              "</body></html>");
}

/* Mapper-only endpoint: persists LED lengths without altering Wi-Fi, IP,
 * GPIOs or colour order. Values are useful/show pixels, not status LEDs. */
static esp_err_t config_pixels_get_handler(httpd_req_t *req)
{
    runtime_config_t cfg = *runtime_config_get();
    bool supplied_any = false;
    uint32_t changed_outputs_mask = 0;
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        char key[8];
        char value[24];
        snprintf(key, sizeof(key), "pix%d", i + 1);
        get_query_value(req, key, value, sizeof(value));
        if (value[0] != '\0') {
            char *end = NULL;
            long pixels = strtol(value, &end, 10);
            if (end == value || *end != '\0' || pixels < 0 || pixels > LED_FLAG_LEDS_PER_OUTPUT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid pixN value");
                return ESP_ERR_INVALID_ARG;
            }
            cfg.pixels_per_output[i] = (uint16_t)pixels;
            supplied_any = true;
            changed_outputs_mask |= 1U << i;
        }
    }

    char active_mask_text[16];
    get_query_value(req, "active_mask", active_mask_text, sizeof(active_mask_text));
    if (active_mask_text[0] != '\0') {
        char *end = NULL;
        unsigned long mask = strtoul(active_mask_text, &end, 10);
        if (end == active_mask_text || *end != '\0' || mask > ((1U << LED_FLAG_OUTPUT_COUNT) - 1U)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid active_mask");
            return ESP_ERR_INVALID_ARG;
        }
        cfg.mapper_active_outputs_mask = (uint32_t)mask;
    } else if (supplied_any) {
        /* Direct partial updates must never leave their own output disabled.
         * The desktop mapper normally supplies the complete outfit mask, but
         * this makes the endpoint safe for a single pixN request too. */
        cfg.mapper_active_outputs_mask |= changed_outputs_mask;
    }
    if (!supplied_any && active_mask_text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "supply pixN or active_mask");
        return ESP_ERR_INVALID_ARG;
    }
    if (!runtime_config_pixels_valid(&cfg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "active outputs exceed 1200 pixels");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = runtime_config_save(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    char response[180];
    const runtime_config_t *active = runtime_config_get();
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"pixels_per_output\":[%u,%u,%u,%u,%u,%u],\"active_outputs_mask\":%" PRIu32 "}",
             active->pixels_per_output[0], active->pixels_per_output[1], active->pixels_per_output[2],
             active->pixels_per_output[3], active->pixels_per_output[4], active->pixels_per_output[5],
             active->mapper_active_outputs_mask);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Reiniciando C5...\n");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static esp_err_t send_sd_control_response(httpd_req_t *req,
                                          spi_link_command_t command,
                                          const char *path,
                                          uint32_t frames,
                                          uint16_t fps,
                                          bool loop,
                                          bool record_show_leds,
                                          bool clock_sync,
                                          const char *ok_text)
{
    spi_master_link_set_frame_pause(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 1; attempt++) {
        ret = spi_master_link_send_sd_control(command, path, frames, fps, loop, record_show_leds, clock_sync);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD proxy command %u enviado intento %d", command, attempt);
        } else {
            ESP_LOGW(TAG,
                     "SD proxy command %u intento %d fallo: %s",
                     command,
                     attempt,
                     esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD proxy command %u fallo: %s", command, esp_err_to_name(ret));
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    switch (command) {
    case SPI_LINK_CMD_SD_RECORD_START:
        s_sd_proxy_recording = true;
        s_sd_proxy_playing = false;
        spi_master_link_set_sd_activity(true, false);
        spi_master_link_set_frame_pause(false);
        s_sd_proxy_recorded_frames = 0;
        s_sd_proxy_dropped_frames = 0;
        if (path != NULL && path[0] != '\0') {
            strlcpy(s_sd_proxy_file, path, sizeof(s_sd_proxy_file));
        }
        break;
    case SPI_LINK_CMD_SD_RECORD_STOP:
        s_sd_proxy_recording = false;
        spi_master_link_set_sd_activity(false, s_sd_proxy_playing);
        if (!s_sd_proxy_playing) {
            spi_master_link_set_frame_pause(false);
        }
        break;
    case SPI_LINK_CMD_SD_PLAY_START:
        s_sd_proxy_playing = true;
        s_sd_proxy_recording = false;
        spi_master_link_set_sd_activity(false, true);
        spi_master_link_set_frame_pause(true);
        if (path != NULL && path[0] != '\0') {
            strlcpy(s_sd_proxy_file, path, sizeof(s_sd_proxy_file));
        }
        break;
    case SPI_LINK_CMD_SD_PLAY_STOP:
        s_sd_proxy_playing = false;
        spi_master_link_set_sd_activity(s_sd_proxy_recording, false);
        if (!s_sd_proxy_recording) {
            spi_master_link_set_frame_pause(false);
        }
        break;
    case SPI_LINK_CMD_SD_DELETE_RECS:
        s_sd_proxy_recording = false;
        s_sd_proxy_playing = false;
        s_sd_proxy_recorded_frames = 0;
        s_sd_proxy_dropped_frames = 0;
        strlcpy(s_sd_proxy_file, "/sdcard/rec1.lfs", sizeof(s_sd_proxy_file));
        spi_master_link_set_sd_activity(false, false);
        spi_master_link_set_frame_pause(false);
        break;
    default:
        spi_master_link_set_frame_pause(false);
        break;
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, ok_text);
}

static esp_err_t sd_status_get_handler(httpd_req_t *req)
{
    spi_link_sd_status_t status = {0};
    esp_err_t ret = spi_master_link_get_sd_status(&status);
    if (ret == ESP_OK) {
        s_sd_proxy_mounted = status.sd_mounted != 0;
        s_sd_proxy_recording = status.recording != 0;
        s_sd_proxy_playing = status.playing != 0;
        s_sd_proxy_recorded_frames = status.recorded_frames;
        s_sd_proxy_dropped_frames = status.record_dropped_frames;
        strlcpy(s_sd_proxy_file, status.file, sizeof(s_sd_proxy_file));
    } else {
        ESP_LOGW(TAG, "No se pudo leer SD status real por MISO: %s", esp_err_to_name(ret));
    }

    char response[256];
    int len = snprintf(response,
                       sizeof(response),
                       "{"
                       "\"sd_mounted\":%s,"
                       "\"recording\":%s,"
                       "\"playing\":%s,"
                       "\"recorded_frames\":%" PRIu32 ","
                       "\"record_dropped_frames\":%" PRIu32 ","
                       "\"file\":\"%s\","
                       "\"proxy\":\"c5_spi\","
                       "\"miso_status\":\"%s\""
                       "}\n",
                       s_sd_proxy_mounted ? "true" : "false",
                       s_sd_proxy_recording ? "true" : "false",
                       s_sd_proxy_playing ? "true" : "false",
                       s_sd_proxy_recorded_frames,
                       s_sd_proxy_dropped_frames,
                       s_sd_proxy_file,
                       ret == ESP_OK ? "ok" : esp_err_to_name(ret));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, len);
}

static esp_err_t sd_list_get_handler(httpd_req_t *req)
{
    spi_link_sd_status_t status = {0};
    esp_err_t ret = spi_master_link_send_sd_control(SPI_LINK_CMD_SD_LIST_RECS,
                                                    NULL,
                                                    0,
                                                    0,
                                                    false,
                                                    false,
                                                    true);
    if (ret == ESP_OK) {
        if (!spi_master_link_last_sd_status(&status)) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo listar SD por MISO: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }

    char files_json[220] = {0};
    char list_copy[SPI_LINK_SD_PATH_MAX] = {0};
    strlcpy(list_copy, status.file, sizeof(list_copy));
    char *save = NULL;
    char *token = strtok_r(list_copy, ",", &save);
    size_t used = 0;
    while (token != NULL && used + 16 < sizeof(files_json)) {
        int written = snprintf(files_json + used,
                               sizeof(files_json) - used,
                               "%s\"/sdcard/%s\"",
                               used > 0 ? "," : "",
                               token);
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
        token = strtok_r(NULL, ",", &save);
    }

    char response[420];
    int len = snprintf(response,
                       sizeof(response),
                       "{\"sd_mounted\":%s,\"mask\":%" PRIu32 ",\"count\":%" PRIu32 ",\"files\":[%s],\"miso_status\":\"ok\"}\n",
                       status.sd_mounted ? "true" : "false",
                       status.recorded_frames,
                       status.record_dropped_frames,
                       files_json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, len);
}

static esp_err_t record_start_get_handler(httpd_req_t *req)
{
    char frames_text[16] = {0};
    char fps_text[16] = {0};
    char show_leds_text[8] = {0};
    char path[SPI_LINK_SD_PATH_MAX] = "/sdcard/test.lfs";
    get_query_value(req, "frames", frames_text, sizeof(frames_text));
    get_query_value(req, "fps", fps_text, sizeof(fps_text));
    get_query_value(req, "show_leds", show_leds_text, sizeof(show_leds_text));
    get_query_value(req, "file", path, sizeof(path));

    uint32_t frames = frames_text[0] != '\0' ? (uint32_t)strtoul(frames_text, NULL, 10) : 0;
    uint16_t fps = fps_text[0] != '\0' ? (uint16_t)strtoul(fps_text, NULL, 10) : 30;
    bool show_leds = strcmp(show_leds_text, "1") == 0 || strcmp(show_leds_text, "true") == 0;
    return send_sd_control_response(req,
                                    SPI_LINK_CMD_SD_RECORD_START,
                                    path,
                                    frames,
                                    fps,
                                    false,
                                    show_leds,
                                    true,
                                    "recording via c5\n");
}

static esp_err_t record_stop_get_handler(httpd_req_t *req)
{
    return send_sd_control_response(req,
                                    SPI_LINK_CMD_SD_RECORD_STOP,
                                    NULL,
                                    0,
                                    0,
                                    false,
                                    false,
                                    true,
                                    "record stopped via c5\n");
}

static esp_err_t play_start_get_handler(httpd_req_t *req)
{
    char fps_text[16] = {0};
    char loop_text[8] = {0};
    char sync_text[8] = {0};
    char path[SPI_LINK_SD_PATH_MAX] = "/sdcard/test.lfs";
    get_query_value(req, "fps", fps_text, sizeof(fps_text));
    get_query_value(req, "loop", loop_text, sizeof(loop_text));
    get_query_value(req, "sync", sync_text, sizeof(sync_text));
    get_query_value(req, "file", path, sizeof(path));

    uint16_t fps = fps_text[0] != '\0' ? (uint16_t)strtoul(fps_text, NULL, 10) : 30;
    bool loop = strcmp(loop_text, "1") == 0 || strcmp(loop_text, "true") == 0;
    bool clock_sync = sync_text[0] == '\0' || strcmp(sync_text, "1") == 0 || strcmp(sync_text, "true") == 0;
    return send_sd_control_response(req,
                                    SPI_LINK_CMD_SD_PLAY_START,
                                    path,
                                    0,
                                    fps,
                                    loop,
                                    false,
                                    clock_sync,
                                    "playing via c5\n");
}

static esp_err_t play_stop_get_handler(httpd_req_t *req)
{
    return send_sd_control_response(req,
                                    SPI_LINK_CMD_SD_PLAY_STOP,
                                    NULL,
                                    0,
                                    0,
                                    false,
                                    false,
                                    true,
                                    "play stopped via c5\n");
}

static esp_err_t sd_delete_recs_get_handler(httpd_req_t *req)
{
    return send_sd_control_response(req,
                                    SPI_LINK_CMD_SD_DELETE_RECS,
                                    NULL,
                                    0,
                                    0,
                                    false,
                                    false,
                                    true,
                                    "deleted rec1..rec10 via c5\n");
}

static esp_err_t rental_on_get_handler(httpd_req_t *req)
{
    esp_err_t ret = rental_control_enable(true);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental on\n");
    return ESP_OK;
}

static esp_err_t rental_off_get_handler(httpd_req_t *req)
{
    esp_err_t ret = rental_control_enable(false);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental off\n");
    return ESP_OK;
}

static esp_err_t rental_play_get_handler(httpd_req_t *req)
{
    esp_err_t ret = rental_control_play_current();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental play\n");
    return ESP_OK;
}

static esp_err_t rental_stop_get_handler(httpd_req_t *req)
{
    esp_err_t ret = rental_control_stop();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental stop\n");
    return ESP_OK;
}

static esp_err_t rental_next_get_handler(httpd_req_t *req)
{
    esp_err_t ret = rental_control_next_show();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental next\n");
    return ESP_OK;
}

static esp_err_t rental_set_get_handler(httpd_req_t *req)
{
    char path[SPI_LINK_SD_PATH_MAX] = CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE;
    get_query_value(req, "file", path, sizeof(path));
    esp_err_t ret = rental_control_set_file(path);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
        return ret;
    }
    httpd_resp_sendstr(req, "rental file set\n");
    return ESP_OK;
}

static esp_err_t send_proxy_command_with_retry(spi_link_command_t command, uint32_t value, int attempts)
{
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        ret = spi_master_link_send_command(command, value);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG,
                 "Proxy command %u intento %d/%d fallo: %s",
                 command,
                 attempt,
                 attempts,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return ret;
}

static esp_err_t cam_ota_proxy_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > (2 * 1024 * 1024)) {
        ESP_LOGW(TAG, "Proxy OTA CAM size invalido=%d", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cam ota image size");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Proxy OTA CAM inicio size=%d", req->content_len);
    spi_master_link_set_frame_pause(true);
    spi_master_link_set_sd_activity(false, false);
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t ret = send_proxy_command_with_retry(SPI_LINK_CMD_PROXY_OTA_BEGIN,
                                                  (uint32_t)req->content_len,
                                                  5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Proxy OTA BEGIN fallo: %s", esp_err_to_name(ret));
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "proxy ota begin failed");
        return ret;
    }

    uint8_t buffer[SPI_LINK_MAX_PAYLOAD_BYTES];
    int remaining = req->content_len;
    uint32_t offset = 0;

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        int received = httpd_req_recv(req, (char *)buffer, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Proxy OTA HTTP recv fallo");
            send_proxy_command_with_retry(SPI_LINK_CMD_PROXY_OTA_ABORT, 0, 2);
            spi_master_link_set_frame_pause(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }

        ret = ESP_FAIL;
        for (int attempt = 1; attempt <= 4; attempt++) {
            ret = spi_master_link_send_ota_chunk(offset,
                                                 buffer,
                                                 (uint16_t)received,
                                                 (uint32_t)req->content_len);
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG,
                     "Proxy OTA chunk retry offset=%" PRIu32 " intento=%d: %s",
                     offset,
                     attempt,
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Proxy OTA chunk fallo offset=%" PRIu32 " size=%d: %s",
                     offset,
                     received,
                     esp_err_to_name(ret));
            send_proxy_command_with_retry(SPI_LINK_CMD_PROXY_OTA_ABORT, 0, 2);
            spi_master_link_set_frame_pause(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spi ota chunk failed");
            return ret;
        }

        offset += (uint32_t)received;
        remaining -= received;
        if ((offset % (64 * 1024)) < (uint32_t)received) {
            ESP_LOGI(TAG, "Proxy OTA CAM enviado %" PRIu32 "/%d bytes", offset, req->content_len);
        }
    }

    ret = send_proxy_command_with_retry(SPI_LINK_CMD_PROXY_OTA_END, offset, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Proxy OTA END fallo: %s", esp_err_to_name(ret));
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "proxy ota end failed");
        return ret;
    }

    ESP_LOGI(TAG, "Proxy OTA CAM completo bytes=%" PRIu32, offset);
    spi_master_link_set_frame_pause(false);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Proxy OTA CAM OK. ESPCAM reiniciando...\n");
}

static uint32_t upload_checksum32_update(uint32_t checksum, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= data[i];
        checksum += 0x9e3779b9U;
    }
    return checksum;
}

static esp_err_t sd_upload_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > (256 * 1024 * 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid lfs size");
        return ESP_ERR_INVALID_SIZE;
    }
    char path[SPI_LINK_SD_PATH_MAX] = "/sdcard/rec1.lfs";
    get_query_value(req, "file", path, sizeof(path));
    if (strncmp(path, "/sdcard/rec", 11) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid lfs path");
        return ESP_ERR_INVALID_ARG;
    }

    spi_master_link_set_frame_pause(true);
    spi_master_link_set_sd_activity(false, false);
    vTaskDelay(pdMS_TO_TICKS(100));

    /*
     * The expected checksum is not known until HTTP has been consumed. Write
     * chunks first to a temporary upload with checksum 0, then restart BEGIN
     * is not possible on a streaming request. The PC supplies it in X-LFS-Checksum.
     */
    char checksum_text[16] = {0};
    size_t checksum_len = httpd_req_get_hdr_value_len(req, "X-LFS-Checksum");
    if (checksum_len == 0 || checksum_len >= sizeof(checksum_text) ||
        httpd_req_get_hdr_value_str(req, "X-LFS-Checksum", checksum_text,
                                    sizeof(checksum_text)) != ESP_OK) {
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-LFS-Checksum");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t expected_checksum = (uint32_t)strtoul(checksum_text, NULL, 16);
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = spi_master_link_send_sd_upload_control(SPI_LINK_CMD_SD_UPLOAD_BEGIN,
                                                     path,
                                                     (uint32_t)req->content_len,
                                                     expected_checksum);
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ret != ESP_OK) {
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd upload begin failed");
        return ret;
    }

    uint8_t buffer[SPI_LINK_MAX_PAYLOAD_BYTES];
    uint32_t offset = 0;
    uint32_t checksum = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int wanted = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        int received = httpd_req_recv(req, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            spi_master_link_send_sd_upload_control(SPI_LINK_CMD_SD_UPLOAD_ABORT, NULL, 0, 0);
            spi_master_link_set_frame_pause(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "http receive failed");
            return ESP_FAIL;
        }
        checksum = upload_checksum32_update(checksum, buffer, received);
        ret = ESP_FAIL;
        for (int attempt = 0; attempt < 5; attempt++) {
            ret = spi_master_link_send_sd_file_chunk(offset,
                                                     buffer,
                                                     (uint16_t)received,
                                                     (uint32_t)req->content_len);
            if (ret == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (ret != ESP_OK) {
            spi_master_link_send_sd_upload_control(SPI_LINK_CMD_SD_UPLOAD_ABORT, NULL, 0, 0);
            spi_master_link_set_frame_pause(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spi file chunk failed");
            return ret;
        }
        offset += received;
        remaining -= received;
    }
    if (checksum != expected_checksum) {
        spi_master_link_send_sd_upload_control(SPI_LINK_CMD_SD_UPLOAD_ABORT, NULL, 0, 0);
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "http checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    ret = spi_master_link_send_sd_upload_control(SPI_LINK_CMD_SD_UPLOAD_END,
                                                  path,
                                                  offset,
                                                  checksum);
    if (ret != ESP_OK) {
        spi_master_link_set_frame_pause(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd upload verify failed");
        return ret;
    }
    spi_link_sd_status_t status = {0};
    ret = spi_master_link_get_sd_status(&status);
    spi_master_link_set_frame_pause(false);
    if (ret != ESP_OK || status.recorded_frames != offset ||
        status.record_dropped_frames != 0 || strcmp(status.file, path) != 0) {
        char detail[220];
        snprintf(detail, sizeof(detail),
                 "sd verify failed bytes=%" PRIu32 "/%" PRIu32
                 " value=%" PRIu32 " file=%s",
                 status.recorded_frames, offset, status.record_dropped_frames, status.file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, detail);
        return ESP_ERR_INVALID_CRC;
    }
    char response[160];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"file\":\"%s\",\"bytes\":%" PRIu32 ",\"checksum\":\"%08" PRIx32 "\"}\n",
             path, offset, checksum);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(update_partition != NULL, ESP_FAIL, TAG, "No OTA partition available");

    ESP_LOGI(TAG, "OTA upload inicio: target=%s size=%d", update_partition->label, req->content_len);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fallo: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buffer[2048];
    int remaining = req->content_len;
    int received_total = 0;

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        int received = httpd_req_recv(req, buffer, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv fallo");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }

        ret = esp_ota_write(ota_handle, buffer, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fallo: %s", esp_err_to_name(ret));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }

        remaining -= received;
        received_total += received;
    }

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end fallo: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return ESP_FAIL;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition fallo: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA completo: bytes=%d. Reiniciando...", received_total);
    httpd_resp_sendstr(req, "OTA OK. Reiniciando...\n");
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t ota_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_uri_handlers = 32;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t test_uri = {
        .uri = "/test",
        .method = HTTP_GET,
        .handler = test_get_handler,
    };
    const httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    const httpd_uri_t config_json_uri = {
        .uri = "/config.json",
        .method = HTTP_GET,
        .handler = config_json_get_handler,
    };
    const httpd_uri_t config_save_uri = {
        .uri = "/config/save",
        .method = HTTP_GET,
        .handler = config_save_get_handler,
    };
    const httpd_uri_t config_pixels_uri = {
        .uri = "/config/pixels",
        .method = HTTP_GET,
        .handler = config_pixels_get_handler,
    };
    const httpd_uri_t reboot_uri = {
        .uri = "/reboot",
        .method = HTTP_GET,
        .handler = reboot_get_handler,
    };
    const httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    const httpd_uri_t fallback_capture_uri = {
        .uri = "/fallback/capture",
        .method = HTTP_GET,
        .handler = fallback_capture_get_handler,
    };
    const httpd_uri_t fallback_clear_uri = {
        .uri = "/fallback/clear",
        .method = HTTP_GET,
        .handler = fallback_clear_get_handler,
    };
    const httpd_uri_t fallback_status_uri = {
        .uri = "/fallback/status",
        .method = HTTP_GET,
        .handler = fallback_status_get_handler,
    };
    const httpd_uri_t cam_ota_on_uri = {
        .uri = "/cam/ota/on",
        .method = HTTP_GET,
        .handler = cam_ota_on_get_handler,
    };
    const httpd_uri_t cam_ota_off_uri = {
        .uri = "/cam/ota/off",
        .method = HTTP_GET,
        .handler = cam_ota_off_get_handler,
    };
    const httpd_uri_t cam_reboot_uri = {
        .uri = "/cam/reboot",
        .method = HTTP_GET,
        .handler = cam_reboot_get_handler,
    };
    const httpd_uri_t cam_ota_proxy_uri = {
        .uri = "/cam/ota/proxy",
        .method = HTTP_POST,
        .handler = cam_ota_proxy_post_handler,
    };
    const httpd_uri_t sd_upload_uri = {
        .uri = "/sd/upload",
        .method = HTTP_POST,
        .handler = sd_upload_post_handler,
    };
    const httpd_uri_t sd_status_uri = {
        .uri = "/sd/status",
        .method = HTTP_GET,
        .handler = sd_status_get_handler,
    };
    const httpd_uri_t sd_list_uri = {
        .uri = "/sd/list",
        .method = HTTP_GET,
        .handler = sd_list_get_handler,
    };
    const httpd_uri_t record_start_uri = {
        .uri = "/record/start",
        .method = HTTP_GET,
        .handler = record_start_get_handler,
    };
    const httpd_uri_t record_stop_uri = {
        .uri = "/record/stop",
        .method = HTTP_GET,
        .handler = record_stop_get_handler,
    };
    const httpd_uri_t play_start_uri = {
        .uri = "/play/start",
        .method = HTTP_GET,
        .handler = play_start_get_handler,
    };
    const httpd_uri_t play_stop_uri = {
        .uri = "/play/stop",
        .method = HTTP_GET,
        .handler = play_stop_get_handler,
    };
    const httpd_uri_t sd_delete_recs_uri = {
        .uri = "/sd/delete_recs",
        .method = HTTP_GET,
        .handler = sd_delete_recs_get_handler,
    };
    const httpd_uri_t rental_on_uri = {
        .uri = "/rental/on",
        .method = HTTP_GET,
        .handler = rental_on_get_handler,
    };
    const httpd_uri_t rental_off_uri = {
        .uri = "/rental/off",
        .method = HTTP_GET,
        .handler = rental_off_get_handler,
    };
    const httpd_uri_t rental_play_uri = {
        .uri = "/rental/play",
        .method = HTTP_GET,
        .handler = rental_play_get_handler,
    };
    const httpd_uri_t rental_stop_uri = {
        .uri = "/rental/stop",
        .method = HTTP_GET,
        .handler = rental_stop_get_handler,
    };
    const httpd_uri_t rental_next_uri = {
        .uri = "/rental/next",
        .method = HTTP_GET,
        .handler = rental_next_get_handler,
    };
    const httpd_uri_t rental_set_uri = {
        .uri = "/rental/set",
        .method = HTTP_GET,
        .handler = rental_set_get_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root_uri), TAG, "root handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status_uri), TAG, "status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &test_uri), TAG, "test handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &config_uri), TAG, "config handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &config_json_uri), TAG, "config json handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &config_save_uri), TAG, "config save handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &config_pixels_uri), TAG, "config pixels handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reboot_uri), TAG, "reboot handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ota_uri), TAG, "ota handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &fallback_capture_uri), TAG, "fallback capture handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &fallback_clear_uri), TAG, "fallback clear handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &fallback_status_uri), TAG, "fallback status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &cam_ota_on_uri), TAG, "cam ota on handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &cam_ota_off_uri), TAG, "cam ota off handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &cam_reboot_uri), TAG, "cam reboot handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &cam_ota_proxy_uri), TAG, "cam ota proxy handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &sd_upload_uri), TAG, "sd upload handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &sd_status_uri), TAG, "sd status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &sd_list_uri), TAG, "sd list handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &record_start_uri), TAG, "record start handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &record_stop_uri), TAG, "record stop handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &play_start_uri), TAG, "play start handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &play_stop_uri), TAG, "play stop handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &sd_delete_recs_uri), TAG, "sd delete recs handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_on_uri), TAG, "rental on handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_off_uri), TAG, "rental off handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_play_uri), TAG, "rental play handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_stop_uri), TAG, "rental stop handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_next_uri), TAG, "rental next handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &rental_set_uri), TAG, "rental set handler failed");

    ESP_LOGI(TAG, "HTTP OTA/control listo: abre http://%s/ o POST /ota", CONFIG_LED_FLAG_WIFI_IP_ADDR);
    if (!s_test_task_handle) {
        xTaskCreate(test_task, "pixel_test", 4096, NULL, 4, &s_test_task_handle);
    }
    return ESP_OK;
}

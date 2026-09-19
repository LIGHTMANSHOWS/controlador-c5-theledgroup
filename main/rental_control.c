#include "rental_control.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "led_flag_common.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_link_protocol.h"
#include "spi_master_link.h"
#include "udp.h"

static const char *TAG = "rental";
static const char *NVS_NS = "rental";

static bool s_enabled;
static int s_slot;
static char s_file[SPI_LINK_SD_PATH_MAX] = CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE;
static TaskHandle_t s_button_task;
static TaskHandle_t s_fallback_task;

#define LIVE_SIGNAL_TIMEOUT_MS 650U
#define FALLBACK_START_TIMEOUT_MS 1500U
#define FALLBACK_RETRY_MS 1000U

#if CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO >= 0
static uint16_t s_label_frame_id = 64000;

static void set_rgb555_pixel(uint8_t *frame, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= LED_FLAG_WIDTH || y < 0 || y >= LED_FLAG_HEIGHT) {
        return;
    }
    uint16_t rgb555 = ((uint16_t)(r >> 3) << 10) |
                      ((uint16_t)(g >> 3) << 5) |
                      (uint16_t)(b >> 3);
    int offset = (y * LED_FLAG_WIDTH + x) * LED_FLAG_BYTES_PER_PIXEL;
    frame[offset] = (rgb555 >> 8) & 0xff;
    frame[offset + 1] = rgb555 & 0xff;
}

static uint8_t font_3x5(char ch, uint8_t row)
{
    static const uint8_t digits[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7},
        {0x2, 0x6, 0x2, 0x2, 0x7},
        {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7},
        {0x5, 0x5, 0x7, 0x1, 0x1},
        {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7},
        {0x7, 0x1, 0x2, 0x2, 0x2},
        {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7},
    };
    if (row >= 5) {
        return 0;
    }
    if (ch >= '0' && ch <= '9') {
        return digits[ch - '0'][row];
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    switch (ch) {
    case 'A': return (const uint8_t[5]){0x2, 0x5, 0x7, 0x5, 0x5}[row];
    case 'B': return (const uint8_t[5]){0x6, 0x5, 0x6, 0x5, 0x6}[row];
    case 'C': return (const uint8_t[5]){0x7, 0x4, 0x4, 0x4, 0x7}[row];
    case 'D': return (const uint8_t[5]){0x6, 0x5, 0x5, 0x5, 0x6}[row];
    case 'E': return (const uint8_t[5]){0x7, 0x4, 0x6, 0x4, 0x7}[row];
    case 'F': return (const uint8_t[5]){0x7, 0x4, 0x6, 0x4, 0x4}[row];
    case 'H': return (const uint8_t[5]){0x5, 0x5, 0x7, 0x5, 0x5}[row];
    case 'I': return row == 0 || row == 4 ? 0x7 : 0x2;
    case 'L': return (const uint8_t[5]){0x4, 0x4, 0x4, 0x4, 0x7}[row];
    case 'N': return (const uint8_t[5]){0x5, 0x7, 0x7, 0x7, 0x5}[row];
    case 'O': return (const uint8_t[5]){0x7, 0x5, 0x5, 0x5, 0x7}[row];
    case 'P': return (const uint8_t[5]){0x6, 0x5, 0x6, 0x4, 0x4}[row];
    case 'R': return (const uint8_t[5]){0x6, 0x5, 0x6, 0x5, 0x5}[row];
    case 'S': return (const uint8_t[5]){0x7, 0x4, 0x7, 0x1, 0x7}[row];
    case 'T': return (const uint8_t[5]){0x7, 0x2, 0x2, 0x2, 0x2}[row];
    case 'W': return (const uint8_t[5]){0x5, 0x5, 0x7, 0x7, 0x5}[row];
    case 'Y': return (const uint8_t[5]){0x5, 0x5, 0x2, 0x2, 0x2}[row];
    case '-': return row == 2 ? 0x7 : 0x0;
    case '_': return row == 4 ? 0x7 : 0x0;
    case '.': return row == 4 ? 0x2 : 0x0;
    default: return 0;
    }
}

static void draw_text_3x5_scaled(uint8_t *frame,
                                 int x0,
                                 int y0,
                                 const char *text,
                                 int scale,
                                 uint8_t r,
                                 uint8_t g,
                                 uint8_t b)
{
    for (int i = 0; text[i] != '\0'; i++) {
        for (int y = 0; y < 5; y++) {
            uint8_t mask = font_3x5(text[i], y);
            for (int x = 0; x < 3; x++) {
                if ((mask & (1 << (2 - x))) == 0) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_rgb555_pixel(frame,
                                         x0 + i * (4 * scale) + x * scale + sx,
                                         y0 + y * scale + sy,
                                         r,
                                         g,
                                         b);
                    }
                }
            }
        }
    }
}

static void file_tail_label(const char *path, char *label, size_t label_size)
{
    if (label == NULL || label_size == 0) {
        return;
    }
    label[0] = '\0';
    const char *name = path != NULL ? strrchr(path, '/') : NULL;
    name = name != NULL ? name + 1 : path;
    if (name == NULL || name[0] == '\0') {
        strlcpy(label, "SHOW", label_size);
        return;
    }

    char clean[24];
    size_t out = 0;
    for (size_t i = 0; name[i] != '\0' && out + 1 < sizeof(clean); i++) {
        if (name[i] == '.') {
            break;
        }
        if ((name[i] >= '0' && name[i] <= '9') ||
            (name[i] >= 'A' && name[i] <= 'Z') ||
            (name[i] >= 'a' && name[i] <= 'z') ||
            name[i] == '_' ||
            name[i] == '-') {
            clean[out++] = name[i];
        }
    }
    clean[out] = '\0';
    if (out == 0) {
        strlcpy(label, "SHOW", label_size);
        return;
    }

    const size_t max_chars = 7;
    const char *tail = out > max_chars ? clean + out - max_chars : clean;
    strlcpy(label, tail, label_size);
}

static void show_button_label(const char *label, uint8_t r, uint8_t g, uint8_t b)
{
    bool previous_recording = spi_master_link_sd_recording();
    bool previous_playing = spi_master_link_sd_playing();
    uint8_t *frame = heap_caps_calloc(1, LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGW(TAG, "Sin memoria para etiqueta boton");
        return;
    }

    for (int y = 0; y < LED_FLAG_HEIGHT; y++) {
        for (int x = 0; x < LED_FLAG_WIDTH; x++) {
            int output = y / LED_FLAG_ROWS_PER_OUTPUT;
            set_rgb555_pixel(frame,
                             x,
                             y,
                             output == 0 ? 12 : 0,
                             output == 1 ? 12 : 0,
                             output == 2 ? 12 : 0);
        }
    }

    int len = (int)strlen(label);
    int scale = 2;
    int text_w = len > 0 ? ((len * 4 - 1) * scale) : 0;
    int x0 = (LED_FLAG_WIDTH - text_w) / 2;
    int y0 = (LED_FLAG_HEIGHT - (5 * scale)) / 2;
    draw_text_3x5_scaled(frame, x0, y0, label, scale, r, g, b);

    spi_master_link_set_sd_activity(previous_recording, true);
    spi_master_link_set_frame_pause(false);
    esp_err_t ret = spi_master_link_queue_frame(++s_label_frame_id, frame);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo mostrar etiqueta %s: %s", label, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Etiqueta boton mostrada: %s", label);
    }
    free(frame);
    vTaskDelay(pdMS_TO_TICKS(1000));
    spi_master_link_set_sd_activity(previous_recording, previous_playing);
}
#endif

static esp_err_t save_state(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open fallo");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs, "enabled", s_enabled ? 1 : 0), out, TAG, "nvs enabled fallo");
    ESP_GOTO_ON_ERROR(nvs_set_i32(nvs, "slot", s_slot), out, TAG, "nvs slot fallo");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, "file", s_file), out, TAG, "nvs file fallo");
    ret = nvs_commit(nvs);
out:
    nvs_close(nvs);
    return ret;
}

static void load_state(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        s_enabled = false;
        s_slot = 0;
        strlcpy(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE, sizeof(s_file));
        return;
    }

    uint8_t enabled = 0;
    int32_t slot = 0;
    size_t file_len = sizeof(s_file);
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) {
        s_enabled = enabled != 0;
    }
    if (nvs_get_i32(nvs, "slot", &slot) == ESP_OK) {
        s_slot = (int)slot;
    }
    if (nvs_get_str(nvs, "file", s_file, &file_len) != ESP_OK || s_file[0] == '\0') {
        strlcpy(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE, sizeof(s_file));
    }
    nvs_close(nvs);

    /* This product intentionally keeps one show only.  Older firmware could
     * persist the non-path sentinel "/sdcard/rec1..rec10.lfs"; migrate it on
     * the first boot so autonomous playback always targets the real file. */
    if (strcmp(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE) != 0) {
        ESP_LOGW(TAG, "Migrando archivo alquiler invalido: %s", s_file);
        strlcpy(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE, sizeof(s_file));
        s_slot = 0;
        esp_err_t save_ret = save_state();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo guardar migracion alquiler: %s", esp_err_to_name(save_ret));
        }
    }
}

static esp_err_t send_play(const char *path)
{
    spi_master_link_set_frame_pause(true);
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_err_t ret = ESP_FAIL;
    bool transmitted = false;
    for (int command_attempt = 1; command_attempt <= 3; command_attempt++) {
        ret = spi_master_link_send_sd_control(SPI_LINK_CMD_SD_PLAY_START,
                                              path,
                                              0,
                                              30,
                                              true,
                                              false,
                                              true);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_RESPONSE) {
            /* INVALID_RESPONSE means the MOSI transfer completed but the
             * optional MISO confirmation was unreadable.  CAM playback start
             * is idempotent, so a bounded resend is safe. */
            transmitted = true;
        }
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    /* The PLAY command response can overlap the CAM transition.  Never report
     * success from the proxy alone: poll the real SD state until CAM confirms
     * mounted+playing. */
    spi_link_sd_status_t status = { 0 };
    for (int attempt = 0; attempt < 6; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_err_t status_ret = spi_master_link_get_sd_status(&status);
        if (status_ret == ESP_OK && status.sd_mounted && status.playing) {
            spi_master_link_set_sd_activity(false, true);
            ESP_LOGI(TAG, "Modo alquiler confirmado: %s", path);
            return ESP_OK;
        }
    }
    if (transmitted) {
        /* The .201 installation has no READY wire and MISO can be noisier
         * than MOSI.  Keep local frames paused after a delivered PLAY command;
         * otherwise the IP fallback is interleaved with valid SD frames. */
        spi_master_link_set_sd_activity(false, true);
        spi_master_link_set_frame_pause(true);
        ESP_LOGW(TAG, "PLAY transmitido sin confirmacion MISO; playback asumido activo: %s", path);
        return ESP_OK;
    }
    spi_master_link_set_frame_pause(false);
    spi_master_link_set_sd_activity(false, false);
    ESP_LOGW(TAG, "CAM no confirmo reproduccion de %s (comando=%s)", path, esp_err_to_name(ret));
    return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
}

esp_err_t rental_control_play_current(void)
{
    return send_play(s_file);
}

esp_err_t rental_control_stop(void)
{
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = spi_master_link_send_sd_control(SPI_LINK_CMD_SD_PLAY_STOP,
                                              NULL,
                                              0,
                                              0,
                                              false,
                                              false,
                                              true);
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    spi_master_link_set_sd_activity(false, false);
    spi_master_link_set_frame_pause(false);
    ESP_LOGI(TAG, "Modo alquiler detenido");
    return ret == ESP_ERR_INVALID_RESPONSE ? ESP_OK : ret;
}

esp_err_t rental_control_enable(bool enable)
{
    s_enabled = enable;
    esp_err_t ret = save_state();
    if (ret != ESP_OK) {
        return ret;
    }
    if (enable) {
        if (udp_receiver_ms_since_last_frame() >= FALLBACK_START_TIMEOUT_MS) {
            return rental_control_play_current();
        }
        return ESP_OK;
    }
    return rental_control_stop();
}

esp_err_t rental_control_set_file(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "archivo alquiler invalido");
    ESP_RETURN_ON_FALSE(strcmp(path, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE) == 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "solo se permite rec1.lfs");
    strlcpy(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE, sizeof(s_file));
    s_slot = 0;
    ESP_RETURN_ON_ERROR(save_state(), TAG, "guardar archivo alquiler fallo");
    ESP_LOGI(TAG, "Archivo alquiler actual: %s", s_file);
    return ESP_OK;
}

esp_err_t rental_control_next_show(void)
{
    s_enabled = true;
    s_slot = 0;
    strlcpy(s_file, CONFIG_LED_FLAG_RENTAL_DEFAULT_FILE, sizeof(s_file));
    ESP_RETURN_ON_ERROR(save_state(), TAG, "guardar slot alquiler fallo");
    return rental_control_play_current();
}

bool rental_control_is_enabled(void)
{
    return s_enabled;
}

const char *rental_control_current_file(void)
{
    return s_file;
}

int rental_control_current_slot(void)
{
    return s_slot;
}

void rental_control_status_json(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) {
        return;
    }
    snprintf(buffer,
             size,
             "\"rental_enabled\":%s,\"rental_slot\":%d,\"rental_file\":\"%s\"",
             s_enabled ? "true" : "false",
             s_slot + 1,
             s_file);
}

static void button_task(void *arg)
{
    (void)arg;
#if CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO >= 0
    /* GPIO9/BOOT may be held low during reset.  Arm only after a stable release
     * so boot cannot be mistaken for a short press that changes the show. */
    TickType_t released_at = xTaskGetTickCount();
    while (true) {
        if (gpio_get_level(CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO) != 0) {
            if ((xTaskGetTickCount() - released_at) * portTICK_PERIOD_MS >= 500) {
                break;
            }
        } else {
            released_at = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bool last_down = false;
    TickType_t pressed_at = 0;

    while (true) {
        bool down = gpio_get_level(CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO) == 0;
        TickType_t now = xTaskGetTickCount();
        if (down && !last_down) {
            pressed_at = now;
        } else if (!down && last_down) {
            uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
            if (held_ms >= CONFIG_LED_FLAG_RENTAL_BUTTON_STOP_MS) {
                ESP_LOGW(TAG, "BOOT largo %" PRIu32 " ms: stop/desactivar alquiler", held_ms);
                s_enabled = false;
                save_state();
                rental_control_stop();
                show_button_label("STOP", 255, 80, 40);
            } else if (held_ms >= CONFIG_LED_FLAG_RENTAL_BUTTON_LONG_MS) {
                ESP_LOGI(TAG, "BOOT largo %" PRIu32 " ms: alternar alquiler", held_ms);
                bool enable = !s_enabled;
                if (enable) {
                    char label[12];
                    rental_control_stop();
                    file_tail_label(s_file, label, sizeof(label));
                    show_button_label(label, 255, 255, 255);
                    rental_control_enable(true);
                } else {
                    rental_control_enable(false);
                    show_button_label("OFF", 255, 80, 40);
                }
            } else if (held_ms >= 40) {
                ESP_LOGI(TAG, "BOOT corto %" PRIu32 " ms: siguiente show", held_ms);
                s_enabled = true;
                save_state();
                rental_control_next_show();
            }
        }
        last_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#else
    vTaskDelete(NULL);
#endif
}

static void fallback_task(void *arg)
{
    (void)arg;
    uint8_t confirmed_stopped = 0;
    uint32_t status_failures = 0;
    vTaskDelay(pdMS_TO_TICKS(1200));
    while (true) {
        if (!s_enabled) {
            confirmed_stopped = 0;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        uint32_t idle_ms = udp_receiver_ms_since_last_frame();
        if (idle_ms < LIVE_SIGNAL_TIMEOUT_MS) {
            confirmed_stopped = 0;
            if (spi_master_link_sd_playing() || spi_master_link_frames_paused()) {
                ESP_LOGI(TAG, "LMP3 activo: volviendo automaticamente a LIVE");
                rental_control_stop();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (idle_ms < FALLBACK_START_TIMEOUT_MS) {
            confirmed_stopped = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        spi_link_sd_status_t status = { 0 };
        esp_err_t status_ret = spi_master_link_get_sd_status(&status);
        if (status_ret != ESP_OK) {
            confirmed_stopped = 0;
            status_failures++;
            if (status_failures == 1 || (status_failures % 20) == 0) {
                ESP_LOGW(TAG,
                         "Estado CAM no disponible (%" PRIu32 "); se conserva playback sin reiniciarlo",
                         status_failures);
            }
            if (status_failures >= 6 && !spi_master_link_sd_playing()) {
                status_failures = 0;
                ESP_LOGW(TAG, "Sin confirmacion MISO: reintentando PLAY de forma controlada");
                rental_control_play_current();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        status_failures = 0;
        if (status.recording) {
            confirmed_stopped = 0;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (status.sd_mounted && status.playing) {
            confirmed_stopped = 0;
            spi_master_link_set_sd_activity(false, true);
            spi_master_link_set_frame_pause(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!status.sd_mounted) {
            confirmed_stopped = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        confirmed_stopped++;
        if (confirmed_stopped < 2) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        confirmed_stopped = 0;

        ESP_LOGI(TAG, "Sin LMP3 por %" PRIu32 " ms: iniciando rec1", idle_ms);
        rental_control_play_current();
        vTaskDelay(pdMS_TO_TICKS(FALLBACK_RETRY_MS));
    }
}

esp_err_t rental_control_init(void)
{
    load_state();
#if CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO >= 0
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "config boton alquiler fallo");
    ESP_LOGI(TAG,
             "Boton alquiler GPIO%d listo. enabled=%u slot=%d file=%s",
             CONFIG_LED_FLAG_RENTAL_BUTTON_GPIO,
             s_enabled ? 1 : 0,
             s_slot + 1,
             s_file);
#else
    ESP_LOGI(TAG, "Boton alquiler desactivado. enabled=%u file=%s", s_enabled ? 1 : 0, s_file);
#endif
    return ESP_OK;
}

void rental_control_start_task(void)
{
    if (s_button_task == NULL) {
        xTaskCreate(button_task, "rental_button", 4096, NULL, 4, &s_button_task);
    }
    if (s_fallback_task == NULL) {
        xTaskCreate(fallback_task, "rental_fallback", 4096, NULL, 3, &s_fallback_task);
    }
}

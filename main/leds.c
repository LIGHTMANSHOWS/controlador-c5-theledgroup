#include "leds.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/parlio_tx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/parlio_types.h"
#include "protocol.h"
#include "runtime_config.h"
#include "soc/soc_caps.h"
#include "udp.h"
#include "wifi.h"

/* ESP32-C5 only has two RMT TX channels, so six independent WS281x outputs are
 * encoded as bits 0..5 of one 8-bit PARLIO TX stream. PARLIO is DMA-backed on
 * C5 and is the right replacement for the old ESP32 "I2S LCD mode" trick. */

static const char *TAG = "leds_parlio";

#define WS_SAMPLE_HZ       2400000U
#define WS_SAMPLES_PER_BIT 3U
#define WS_RESET_US        100U
#define WS_TX_WAIT_MS       50U /* 18.1 ms de señal a 601 LEDs + margen de scheduler */
#define WS_DATA_SAMPLES    (LED_FLAG_PHYSICAL_LEDS_PER_OUTPUT * 3U * 8U * WS_SAMPLES_PER_BIT)
#define WS_RESET_SAMPLES   ((WS_SAMPLE_HZ / 1000000U) * WS_RESET_US)
#define WS_DMA_BYTES       (WS_DATA_SAMPLES + WS_RESET_SAMPLES)
#define WS_OUTPUT_MASK     ((1U << LED_FLAG_OUTPUT_COUNT) - 1U)
#define STATUS_REFRESH_MS  120U

static parlio_tx_unit_handle_t s_tx_unit;
static uint8_t *s_dma_buffer;
static uint8_t *s_pending_frame;
static bool s_pending_limit_enabled;
static uint8_t s_pending_limit = 255;
static SemaphoreHandle_t s_frame_lock;
static SemaphoreHandle_t s_frame_ready;
static TaskHandle_t s_led_task;
static volatile uint32_t s_output_frames;
static volatile uint32_t s_dropped_frames;
static volatile uint32_t s_last_output_us;
static volatile uint32_t s_output_fps;
static uint32_t s_fps_frames;
static int64_t s_fps_started_us;

static uint16_t rgb555_make(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 10) | ((uint16_t)(g >> 3) << 5) | (uint16_t)(b >> 3);
}

static uint16_t status_pixel_rgb555(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool blink_fast = ((now_ms / 220) & 1) == 0;
    const bool blink_slow = ((now_ms / 450) & 1) == 0;

    switch (wifi_link_state()) {
    case WIFI_LINK_CONNECTED: {
        uint32_t udp_idle_ms = udp_receiver_ms_since_last_frame();
        if (udp_idle_ms < 250) {
            return blink_fast ? rgb555_make(0, 64, 0) : rgb555_make(0, 8, 0);
        }
        return rgb555_make(0, 42, 0);
    }
    case WIFI_LINK_RETRYING:
        return blink_slow ? rgb555_make(64, 42, 0) : 0; /* amarillo parpadeando */
    case WIFI_LINK_FAILED:
        return rgb555_make(64, 0, 0); /* rojo */
    case WIFI_LINK_CONNECTING:
    default:
        return rgb555_make(64, 24, 0); /* anaranjado */
    }
}

static inline void rgb555_expand(uint16_t word, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (word >> 10) & 0x1f;
    uint8_t g5 = (word >> 5) & 0x1f;
    uint8_t b5 = word & 0x1f;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g5 << 3) | (g5 >> 2);
    *b = (b5 << 3) | (b5 >> 2);
}

static inline uint8_t capped_level(uint8_t level, const uint8_t base[32],
                                    const uint8_t remainder[32], uint32_t phase,
                                    uint32_t pixel, uint8_t channel)
{
    uint8_t value = base[level];
    uint8_t threshold = (uint8_t)((phase * 13U + pixel * 7U + channel * 17U) & 31U);
    if ((uint16_t)remainder[level] * 32U > (uint16_t)threshold * 31U && value < 255) {
        ++value;
    }
    return value;
}

static esp_err_t rgb555_gray_self_test(void)
{
    for (uint16_t level = 0; level < 32; ++level) {
        uint16_t word = (level << 10) | (level << 5) | level;
        uint8_t r, g, b;
        rgb555_expand(word, &r, &g, &b);
        if (r != g || g != b) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void encode_parallel_rgb555(const uint8_t *frame, bool limit_enabled, uint8_t limit)
{
    uint8_t *dst = s_dma_buffer;
    memset(dst, 0, WS_DMA_BYTES);

    const runtime_config_t *cfg = runtime_config_get();
    const uint16_t status_word = status_pixel_rgb555();
    const uint32_t phase = s_output_frames;
    uint8_t capped_base[32] = {0};
    uint8_t capped_remainder[32] = {0};
    if (limit_enabled) {
        for (int level = 0; level < 32; ++level) {
            uint16_t numerator = (uint16_t)level * limit;
            capped_base[level] = (uint8_t)(numerator / 31U);
            capped_remainder[level] = (uint8_t)(numerator % 31U);
        }
    }
    uint16_t output_offset[LED_FLAG_OUTPUT_COUNT] = {0};
    for (int out = 1; out < LED_FLAG_OUTPUT_COUNT; ++out) {
        output_offset[out] = output_offset[out - 1] +
            (runtime_config_output_active(cfg, out - 1) ? cfg->pixels_per_output[out - 1] : 0);
    }

    for (int pixel = 0; pixel < LED_FLAG_PHYSICAL_LEDS_PER_OUTPUT; ++pixel) {
        uint8_t colors[LED_FLAG_OUTPUT_COUNT][3];
        for (int out = 0; out < LED_FLAG_OUTPUT_COUNT; ++out) {
            uint16_t active_pixels = runtime_config_output_active(cfg, out) ? cfg->pixels_per_output[out] : 0;
            if (active_pixels > LED_FLAG_LEDS_PER_OUTPUT) {
                active_pixels = LED_FLAG_LEDS_PER_OUTPUT;
            }
            uint16_t word = 0;
            if (pixel == 0) {
                word = status_word;
            } else if (pixel <= active_pixels) {
                int logical_pixel = pixel - 1;
                int index = output_offset[out] + logical_pixel;
                word = ((uint16_t)frame[index * 2] << 8) | frame[index * 2 + 1];
            }
            uint8_t r, g, b;
            if (limit_enabled && pixel > 0) {
                uint32_t physical_pixel = (uint32_t)out * LED_FLAG_PHYSICAL_LEDS_PER_OUTPUT + pixel;
                r = capped_level((word >> 10) & 0x1f, capped_base, capped_remainder, phase, physical_pixel, 0);
                g = capped_level((word >> 5) & 0x1f, capped_base, capped_remainder, phase, physical_pixel, 1);
                b = capped_level(word & 0x1f, capped_base, capped_remainder, phase, physical_pixel, 2);
            } else {
                rgb555_expand(word, &r, &g, &b);
            }
            switch (cfg->color_order) {
            case RUNTIME_CONFIG_COLOR_RGB:
                colors[out][0] = r; colors[out][1] = g; colors[out][2] = b; break;
            case RUNTIME_CONFIG_COLOR_RBG:
                colors[out][0] = r; colors[out][1] = b; colors[out][2] = g; break;
            case RUNTIME_CONFIG_COLOR_GBR:
                colors[out][0] = g; colors[out][1] = b; colors[out][2] = r; break;
            case RUNTIME_CONFIG_COLOR_BRG:
                colors[out][0] = b; colors[out][1] = r; colors[out][2] = g; break;
            case RUNTIME_CONFIG_COLOR_BGR:
                colors[out][0] = b; colors[out][1] = g; colors[out][2] = r; break;
            default:
                colors[out][0] = g; colors[out][1] = r; colors[out][2] = b; break;
            }
        }

        for (int component = 0; component < 3; ++component) {
            for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
                uint8_t ones = 0;
                for (int out = 0; out < LED_FLAG_OUTPUT_COUNT; ++out) {
                    if ((colors[out][component] & mask) != 0) {
                        ones |= (uint8_t)(1U << out);
                    }
                }
                *dst++ = (uint8_t)WS_OUTPUT_MASK;
                *dst++ = ones;
                *dst++ = 0x00;
            }
        }
    }
}

static esp_err_t transmit_and_wait(void)
{
    parlio_transmit_config_t tx_config = {
        .idle_value = 0x00,
    };
    ESP_RETURN_ON_ERROR(parlio_tx_unit_transmit(s_tx_unit,
                                                s_dma_buffer,
                                                WS_DMA_BYTES * 8U,
                                                &tx_config),
                        TAG,
                        "parlio transmit");
    return parlio_tx_unit_wait_all_done(s_tx_unit, WS_TX_WAIT_MS);
}

static esp_err_t init_parallel_parlio(void)
{
#if !SOC_PARLIO_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_dma_buffer = heap_caps_calloc(1, WS_DMA_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_dma_buffer, ESP_ERR_NO_MEM, TAG, "sin buffer DMA");

    parlio_tx_unit_config_t config = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .output_clk_freq_hz = WS_SAMPLE_HZ,
        .data_width = 8,
        .data_gpio_nums = {
            runtime_config_get()->led_gpio[0],
            runtime_config_get()->led_gpio[1],
            runtime_config_get()->led_gpio[2],
            runtime_config_get()->led_gpio[3],
            runtime_config_get()->led_gpio[4],
            runtime_config_get()->led_gpio[5],
            -1,
            -1,
        },
        .trans_queue_depth = 2,
        .max_transfer_size = WS_DMA_BYTES,
        .dma_burst_size = 32,
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    ESP_RETURN_ON_ERROR(parlio_new_tx_unit(&config, &s_tx_unit), TAG, "parlio tx");
    ESP_RETURN_ON_ERROR(parlio_tx_unit_enable(s_tx_unit), TAG, "parlio enable");

    const runtime_config_t *cfg = runtime_config_get();
    for (int i = 0; i < LED_FLAG_OUTPUT_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability((gpio_num_t)cfg->led_gpio[i], GPIO_DRIVE_CAP_3),
                            TAG,
                            "drive gpio");
    }
    return ESP_OK;
#endif
}

static void led_task(void *arg)
{
    (void)arg;
    uint8_t *local = heap_caps_malloc(LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    if (!local) {
        vTaskDelete(NULL);
        return;
    }

    memset(local, 0, LED_FLAG_FRAME_SIZE_BYTES);
    bool local_limit_enabled = false;
    uint8_t local_limit = 255;

    while (true) {
        if (xSemaphoreTake(s_frame_ready, pdMS_TO_TICKS(STATUS_REFRESH_MS)) == pdTRUE) {
            xSemaphoreTake(s_frame_lock, portMAX_DELAY);
            memcpy(local, s_pending_frame, LED_FLAG_FRAME_SIZE_BYTES);
            local_limit_enabled = s_pending_limit_enabled;
            local_limit = s_pending_limit;
            xSemaphoreGive(s_frame_lock);
        } else {
            memset(local, 0, LED_FLAG_FRAME_SIZE_BYTES);
        }

        int64_t start = esp_timer_get_time();
        encode_parallel_rgb555(local, local_limit_enabled, local_limit);
        if (transmit_and_wait() == ESP_OK) {
            s_output_frames++;
            s_last_output_us = (uint32_t)(esp_timer_get_time() - start);
            s_fps_frames++;
            int64_t now = esp_timer_get_time();
            if (s_fps_started_us == 0) {
                s_fps_started_us = now;
            }
            if (now - s_fps_started_us >= 1000000) {
                s_output_fps = (uint32_t)((s_fps_frames * 1000000ULL) / (now - s_fps_started_us));
                s_fps_frames = 0;
                s_fps_started_us = now;
            }
        } else {
            s_dropped_frames++;
        }
    }
}

esp_err_t leds_init(void)
{
    ESP_RETURN_ON_ERROR(rgb555_gray_self_test(), TAG, "RGB555 gray self-test");
    s_frame_lock = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();
    s_pending_frame = heap_caps_calloc(1, LED_FLAG_FRAME_SIZE_BYTES, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_frame_lock && s_frame_ready && s_pending_frame,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "sin buffers/semaforos");
    ESP_RETURN_ON_ERROR(init_parallel_parlio(), TAG, "PARLIO/GDMA");
    ESP_LOGI(TAG, "ESP32-C5 PARLIO: 6 x 200 WS281x; RGB555 simetrico OK");
    return ESP_OK;
}

void leds_start_task(void)
{
    if (!s_led_task) {
        xTaskCreate(led_task, "led_parlio", 4096, NULL, 9, &s_led_task);
    }
}

esp_err_t leds_commit_frame(const uint8_t *frame)
{
    return leds_commit_frame_with_brightness(frame, false, 255);
}

esp_err_t leds_commit_frame_with_brightness(const uint8_t *frame, bool enabled, uint8_t limit)
{
    if (!frame || !s_pending_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        s_dropped_frames++;
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_pending_frame, frame, LED_FLAG_FRAME_SIZE_BYTES);
    s_pending_limit_enabled = enabled;
    s_pending_limit = limit;
    xSemaphoreGive(s_frame_lock);
    xSemaphoreGive(s_frame_ready);
    return ESP_OK;
}

bool leds_brightness_limit_active(void) { return s_pending_limit_enabled; }
uint8_t leds_brightness_limit_value(void) { return s_pending_limit; }

esp_err_t leds_commit_logical_frame(const uint8_t *frame) { return leds_commit_frame(frame); }
esp_err_t leds_clear(void) { uint8_t blank[LED_FLAG_FRAME_SIZE_BYTES] = {0}; return leds_commit_frame(blank); }
void leds_show_frame(const uint8_t *frame) { (void)leds_commit_frame(frame); }

void leds_show_test_pattern(void)
{
    uint8_t frame[LED_FLAG_FRAME_SIZE_BYTES] = {0};
    const runtime_config_t *cfg = runtime_config_get();
    int offset = 0;
    for (int out = 0; out < LED_FLAG_OUTPUT_COUNT; ++out) {
        int count = runtime_config_output_active(cfg, out) ? cfg->pixels_per_output[out] : 0;
        for (int p = 0; p < count; ++p) {
            uint8_t level = count < 2 ? 31 : (uint8_t)((p * 31) / (count - 1));
            uint16_t gray = (level << 10) | (level << 5) | level;
            int i = (offset + p) * 2;
            frame[i] = (uint8_t)(gray >> 8);
            frame[i + 1] = (uint8_t)gray;
        }
        offset += count;
    }
    (void)leds_commit_frame(frame);
}

uint32_t leds_output_frames(void) { return s_output_frames; }
uint32_t leds_dropped_frames(void) { return s_dropped_frames; }
uint32_t leds_last_output_us(void) { return s_last_output_us; }
uint32_t leds_output_fps(void) { return s_output_fps; }

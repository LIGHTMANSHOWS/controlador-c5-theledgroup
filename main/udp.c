#include "udp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "frame_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "protocol.h"

#define FRAME_ASSEMBLY_TIMEOUT_MS 40
#define STREAM_RESTART_TIMEOUT_MS 120 /* permite reiniciar un sender sin reiniciar el C5 */

static const char *TAG = "udp";
static TaskHandle_t s_udp_task_handle;
static volatile TickType_t s_last_valid_packet_tick;
static volatile TickType_t s_last_completed_frame_tick;
static volatile bool s_received_complete_frame;
static volatile uint32_t s_valid_packets;
static volatile uint32_t s_invalid_packets;
static volatile uint32_t s_completed_frames;
static volatile uint32_t s_incomplete_frames;
static volatile uint32_t s_discarded_frames;
static volatile uint32_t s_lost_packets;
static volatile uint16_t s_last_frame_id;
static volatile uint32_t s_last_assembly_us;
static volatile uint32_t s_rx_fps;
static uint32_t s_fps_frames;
static int64_t s_fps_started_us;

typedef struct {
    struct sockaddr_storage address;
    socklen_t length;
    bool valid;
} udp_source_t;

typedef struct {
    uint16_t frame_id;
    uint8_t received_mask;
    uint8_t received_packets;
    uint8_t flags;
    uint8_t brightness_limit;
    int64_t started_us;
    bool active;
    udp_source_t source;
} frame_assembly_t;

static bool source_equals(const udp_source_t *saved, const struct sockaddr *incoming, socklen_t incoming_len)
{
    if (!saved->valid || incoming == NULL || incoming_len < sizeof(sa_family_t) ||
        saved->address.ss_family != incoming->sa_family) {
        return false;
    }
    if (incoming->sa_family == AF_INET && saved->length >= sizeof(struct sockaddr_in) &&
        incoming_len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *left = (const struct sockaddr_in *)&saved->address;
        const struct sockaddr_in *right = (const struct sockaddr_in *)incoming;
        return left->sin_addr.s_addr == right->sin_addr.s_addr && left->sin_port == right->sin_port;
    }
    return saved->length == incoming_len && memcmp(&saved->address, incoming, incoming_len) == 0;
}

static void source_copy(udp_source_t *destination, const struct sockaddr *source, socklen_t source_len)
{
    memset(destination, 0, sizeof(*destination));
    if (source == NULL || source_len == 0 || source_len > sizeof(destination->address)) return;
    memcpy(&destination->address, source, source_len);
    destination->length = source_len;
    destination->valid = true;
}

static bool frame_id_is_newer(uint16_t candidate, uint16_t reference)
{
    uint16_t delta = (uint16_t)(candidate - reference);
    return delta != 0 && delta < 0x8000;
}

static void send_ack(int sock, const struct sockaddr *source, socklen_t source_len,
                     const led_flag_packet_header_t *header, led_flag_ack_status_t status,
                     uint16_t received)
{
    led_flag_ack_t ack = {
        .magic = LED_FLAG_ACK_MAGIC,
        .version = LED_FLAG_PROTOCOL_VERSION,
        .matrix_id = CONFIG_LED_FLAG_MATRIX_ID,
        .frame_id = header->frame_id,
        .packet_id = header->packet_id,
        .packet_count = header->packet_count,
        .received_packets = received,
        .status = status,
    };
    (void)sendto(sock, &ack, sizeof(ack), 0, source, source_len);
}

static bool validate_packet(const uint8_t *packet, int packet_len)
{
    if (packet_len < (int)sizeof(led_flag_packet_header_t)) return false;
    const led_flag_packet_header_t *h = (const led_flag_packet_header_t *)packet;
    if (memcmp(h->magic, LED_FLAG_PROTOCOL_MAGIC, 4) != 0 ||
        h->version != LED_FLAG_PROTOCOL_VERSION ||
        h->pixel_format != LED_FLAG_PIXEL_FORMAT_RGB555_BE ||
        (h->flags & ~LED_FLAG_FLAG_BRIGHTNESS_LIMIT) != 0 ||
        (!(h->flags & LED_FLAG_FLAG_BRIGHTNESS_LIMIT) && h->reserved != 0) ||
        h->packet_count != LED_FLAG_PACKET_COUNT ||
        h->packet_id >= LED_FLAG_PACKET_COUNT ||
        h->pixel_count != LED_FLAG_PIXELS_PER_PACKET ||
        h->pixel_offset != h->packet_id * LED_FLAG_PIXELS_PER_PACKET ||
        h->payload_size != LED_FLAG_PACKET_PAYLOAD_BYTES ||
        packet_len != (int)(sizeof(*h) + h->payload_size)) {
        return false;
    }
    const uint8_t *payload = packet + sizeof(*h);
    return led_flag_checksum16(payload, h->payload_size) == h->checksum;
}

static void discard_incomplete(frame_assembly_t *a)
{
    if (!a->active) return;
    s_incomplete_frames++;
    s_discarded_frames++;
    s_lost_packets += LED_FLAG_PACKET_COUNT - a->received_packets;
    a->active = false;
}

static void begin_frame(frame_assembly_t *a, const led_flag_packet_header_t *h,
                        const struct sockaddr *source, socklen_t source_len)
{
    a->frame_id = h->frame_id;
    a->received_mask = 0;
    a->received_packets = 0;
    a->flags = h->flags;
    a->brightness_limit = h->reserved;
    a->started_us = esp_timer_get_time();
    a->active = true;
    source_copy(&a->source, source, source_len);
}

static void handle_packet(frame_assembly_t *a, int sock,
                          const struct sockaddr *source, socklen_t source_len,
                          const uint8_t *packet, int packet_len, udp_source_t *last_source)
{
    if (!validate_packet(packet, packet_len)) {
        s_invalid_packets++;
        return;
    }
    const led_flag_packet_header_t *h = (const led_flag_packet_header_t *)packet;
    const uint8_t *payload = packet + sizeof(*h);
    s_valid_packets++;
    s_last_valid_packet_tick = xTaskGetTickCount();

    if (!a->active) {
        const bool new_sender = !source_equals(last_source, source, source_len);
        const bool sender_restarted = s_received_complete_frame &&
            (xTaskGetTickCount() - s_last_completed_frame_tick) * portTICK_PERIOD_MS >= STREAM_RESTART_TIMEOUT_MS;
        if (s_received_complete_frame && !new_sender && !sender_restarted &&
            !frame_id_is_newer(h->frame_id, s_last_frame_id)) {
            s_discarded_frames++;
            return;
        }
        begin_frame(a, h, source, source_len);
    } else if (!source_equals(&a->source, source, source_len)) {
        /* Otro PC tomó el control: no mezclamos sus paquetes con el cuadro previo. */
        discard_incomplete(a);
        begin_frame(a, h, source, source_len);
    } else if (h->frame_id != a->frame_id) {
        if (!frame_id_is_newer(h->frame_id, a->frame_id)) {
            s_discarded_frames++;
            return;
        }
        discard_incomplete(a);
        begin_frame(a, h, source, source_len);
    }

    if (h->flags != a->flags || h->reserved != a->brightness_limit) {
        s_invalid_packets++;
        discard_incomplete(a);
        return;
    }

    uint8_t bit = (uint8_t)(1U << h->packet_id);
    if (a->received_mask & bit) return;
    memcpy(frame_buffer_get_receive_buffer() + h->pixel_offset * 2, payload, h->payload_size);
    a->received_mask |= bit;
    a->received_packets++;
    send_ack(sock, source, source_len, h, LED_FLAG_ACK_PACKET_RECEIVED, a->received_packets);

    if (a->received_mask == 0x03) {
        s_last_assembly_us = (uint32_t)(esp_timer_get_time() - a->started_us);
        esp_err_t ret = frame_buffer_commit_receive_buffer(
            h->frame_id, (a->flags & LED_FLAG_FLAG_BRIGHTNESS_LIMIT) != 0, a->brightness_limit);
        if (ret == ESP_OK) {
            s_completed_frames++;
            s_fps_frames++;
            int64_t now_us = esp_timer_get_time();
            if (s_fps_started_us == 0) s_fps_started_us = now_us;
            if (now_us - s_fps_started_us >= 1000000) {
                s_rx_fps = (uint32_t)((s_fps_frames * 1000000ULL) / (now_us - s_fps_started_us));
                s_fps_frames = 0;
                s_fps_started_us = now_us;
            }
            s_last_frame_id = h->frame_id;
            s_last_completed_frame_tick = xTaskGetTickCount();
            s_received_complete_frame = true;
            *last_source = a->source;
            send_ack(sock, source, source_len, h, LED_FLAG_ACK_FRAME_COMPLETE, 2);
        } else {
            s_discarded_frames++;
            send_ack(sock, source, source_len, h, LED_FLAG_ACK_FRAME_DROPPED, 2);
        }
        a->active = false;
    }
}

static void udp_task(void *arg)
{
    (void)arg;
    uint8_t packet_buffer[LED_FLAG_MAX_PACKET_BYTES];
    frame_assembly_t assembly = {0};
    udp_source_t last_source = {0};
    while (true) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 3000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int rcvbuf = 64 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        struct sockaddr_in listen_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(LED_FLAG_UDP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            close(sock); vTaskDelay(pdMS_TO_TICKS(500)); continue;
        }
        ESP_LOGI(TAG, "UDP unicast %d: 2 paquetes x 600 pixeles RGB555", LED_FLAG_UDP_PORT);
        while (true) {
            struct sockaddr_storage source;
            socklen_t source_len = sizeof(source);
            int len = recvfrom(sock, packet_buffer, sizeof(packet_buffer), 0,
                               (struct sockaddr *)&source, &source_len);
            if (len >= 0) {
                handle_packet(&assembly, sock, (const struct sockaddr *)&source,
                              source_len, packet_buffer, len, &last_source);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            } else {
                vTaskDelay(1);
            }
            if (assembly.active &&
                esp_timer_get_time() - assembly.started_us > FRAME_ASSEMBLY_TIMEOUT_MS * 1000LL) {
                discard_incomplete(&assembly);
            }
        }
        close(sock);
    }
}

esp_err_t udp_receiver_init(void) { s_last_valid_packet_tick = xTaskGetTickCount(); return ESP_OK; }
void udp_receiver_start_task(void) { if (!s_udp_task_handle) xTaskCreate(udp_task, "udp", 6144, NULL, 8, &s_udp_task_handle); }
uint32_t udp_receiver_ms_since_last_packet(void) { return (xTaskGetTickCount() - s_last_valid_packet_tick) * portTICK_PERIOD_MS; }
uint32_t udp_receiver_ms_since_last_frame(void) { return s_received_complete_frame ? (xTaskGetTickCount() - s_last_completed_frame_tick) * portTICK_PERIOD_MS : UINT32_MAX; }
uint32_t udp_receiver_valid_packets(void) { return s_valid_packets; }
uint32_t udp_receiver_invalid_packets(void) { return s_invalid_packets; }
uint32_t udp_receiver_completed_frames(void) { return s_completed_frames; }
uint32_t udp_receiver_incomplete_frames(void) { return s_incomplete_frames; }
uint32_t udp_receiver_discarded_frames(void) { return s_discarded_frames; }
uint32_t udp_receiver_detected_lost_packets(void) { return s_lost_packets; }
uint16_t udp_receiver_last_frame_id(void) { return s_last_frame_id; }
uint32_t udp_receiver_last_assembly_us(void) { return s_last_assembly_us; }
uint32_t udp_receiver_rx_fps(void) { return s_rx_fps; }

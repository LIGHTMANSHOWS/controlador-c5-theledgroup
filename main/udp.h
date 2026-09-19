#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t udp_receiver_init(void);
void udp_receiver_start_task(void);
uint32_t udp_receiver_ms_since_last_packet(void);
uint32_t udp_receiver_ms_since_last_frame(void);
uint32_t udp_receiver_valid_packets(void);
uint32_t udp_receiver_invalid_packets(void);
uint32_t udp_receiver_completed_frames(void);
uint32_t udp_receiver_incomplete_frames(void);
uint32_t udp_receiver_discarded_frames(void);
uint32_t udp_receiver_detected_lost_packets(void);
uint16_t udp_receiver_last_frame_id(void);
uint32_t udp_receiver_last_assembly_us(void);
uint32_t udp_receiver_rx_fps(void);

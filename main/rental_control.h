#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t rental_control_init(void);
void rental_control_start_task(void);

esp_err_t rental_control_enable(bool enable);
esp_err_t rental_control_play_current(void);
esp_err_t rental_control_stop(void);
esp_err_t rental_control_next_show(void);
esp_err_t rental_control_set_file(const char *path);

bool rental_control_is_enabled(void);
const char *rental_control_current_file(void);
int rental_control_current_slot(void);
void rental_control_status_json(char *buffer, size_t size);

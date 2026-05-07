#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool recording;
    bool has_recording;
    size_t file_size;
    uint32_t duration_ms;
    uint32_t sample_rate_hz;
} recording_status_t;

esp_err_t recording_service_init(void);
esp_err_t recording_service_trigger(void);
void recording_service_get_status(recording_status_t *status);
const char *recording_service_get_file_path(void);

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool recording;
    bool has_recording;
    size_t file_size;
    size_t expected_file_size;
    uint32_t record_id;
    uint32_t active_record_id;
    uint32_t duration_ms;
    uint32_t sample_rate_hz;
    char path[40];
    char download_name[32];
} recording_status_t;

esp_err_t recording_service_init(void);
esp_err_t recording_service_trigger(void);
void recording_service_get_status(recording_status_t *status);
const char *recording_service_get_file_path(void);

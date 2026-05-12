#pragma once

#include "esp_err.h"

#include <stdbool.h>

esp_err_t portal_service_init(void);
esp_err_t portal_service_start(void);
esp_err_t portal_service_start_minimal_wifi(void);
esp_err_t portal_service_stop(void);
esp_err_t portal_service_toggle(void);
bool portal_service_is_running(void);

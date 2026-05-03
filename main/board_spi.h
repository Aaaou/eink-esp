#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

esp_err_t board_eink_spi_init(spi_device_handle_t *out_dev);

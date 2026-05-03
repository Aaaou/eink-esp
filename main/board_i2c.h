#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

esp_err_t board_i2c_init(void);
esp_err_t board_i2c_write_bytes(uint8_t dev_addr, const uint8_t *data, size_t len);
esp_err_t board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);
esp_err_t board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value);
bool board_i2c_probe(uint8_t dev_addr);
void board_i2c_scan(void);

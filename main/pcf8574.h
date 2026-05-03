#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t address;
    uint8_t latch;
    bool initialized;
} pcf8574_t;

esp_err_t pcf8574_init(pcf8574_t *dev, uint8_t addr, uint8_t initial_latch);
esp_err_t pcf8574_write(pcf8574_t *dev, uint8_t value);
esp_err_t pcf8574_set_bit(pcf8574_t *dev, uint8_t bit, bool high);
uint8_t pcf8574_get_latch(const pcf8574_t *dev);

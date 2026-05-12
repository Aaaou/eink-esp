#include "pcf8574.h"

#include "board_i2c.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcf8574";

esp_err_t pcf8574_init(pcf8574_t *dev, uint8_t addr, uint8_t initial_latch)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev is null");
    dev->address = addr;
    dev->latch = initial_latch;
    dev->initialized = false;

    ESP_RETURN_ON_ERROR(pcf8574_write(dev, initial_latch), TAG, "initial write failed");
    dev->initialized = true;
    ESP_LOGI(TAG, "[√] PCF8574 在线: 0x%02X", dev->address);
    return ESP_OK;
}

esp_err_t pcf8574_write(pcf8574_t *dev, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev is null");
    ESP_RETURN_ON_ERROR(board_i2c_write_bytes(dev->address, &value, 1), TAG, "write failed");
    dev->latch = value;
    return ESP_OK;
}

esp_err_t pcf8574_set_bit(pcf8574_t *dev, uint8_t bit, bool high)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev is null");
    ESP_RETURN_ON_FALSE(bit < 8, ESP_ERR_INVALID_ARG, TAG, "bit out of range");

    uint8_t next = dev->latch;
    if (high) {
        next |= (uint8_t)(1U << bit);
    } else {
        next &= (uint8_t)~(1U << bit);
    }
    return pcf8574_write(dev, next);
}

uint8_t pcf8574_get_latch(const pcf8574_t *dev)
{
    return dev ? dev->latch : 0xFF;
}

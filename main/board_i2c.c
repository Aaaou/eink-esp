#include "board_i2c.h"

#include "board_config.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "board_i2c";
static bool s_i2c_ready;

esp_err_t board_i2c_init(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }

    const i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(BOARD_I2C_PORT, &cfg), TAG, "i2c_param_config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(BOARD_I2C_PORT, cfg.mode, 0, 0, 0), TAG, "i2c_driver_install failed");

    s_i2c_ready = true;
    ESP_LOGI(TAG, "I2C ready: port=%d scl=%d sda=%d freq=%d",
             BOARD_I2C_PORT, BOARD_I2C_SCL_GPIO, BOARD_I2C_SDA_GPIO, BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t board_i2c_write_bytes(uint8_t dev_addr, const uint8_t *data, size_t len)
{
    return i2c_master_write_to_device(BOARD_I2C_PORT, dev_addr, data, len, pdMS_TO_TICKS(100));
}

esp_err_t board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t value)
{
    const uint8_t payload[] = { reg_addr, value };
    return board_i2c_write_bytes(dev_addr, payload, sizeof(payload));
}

esp_err_t board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value)
{
    return i2c_master_write_read_device(BOARD_I2C_PORT, dev_addr, &reg_addr, 1, value, 1, pdMS_TO_TICKS(100));
}

bool board_i2c_probe(uint8_t dev_addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return false;
    }

    esp_err_t err = i2c_master_start(cmd);
    if (err == ESP_OK) {
        err = i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    }
    if (err == ESP_OK) {
        err = i2c_master_stop(cmd);
    }
    if (err == ESP_OK) {
        err = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    }

    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

void board_i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus for responding devices");
    for (uint8_t addr = 1; addr < 0x78; ++addr) {
        if (board_i2c_probe(addr)) {
            ESP_LOGI(TAG, "I2C device detected at 0x%02X", addr);
        }
    }
}

#include "board_bringup.h"

#include "board_i2c.h"
#include "eink_bringup.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_INK_RUN_EINK_BRINGUP_ON_BOOT
#define CONFIG_INK_RUN_EINK_BRINGUP_ON_BOOT 0
#endif

static const char *TAG = "board_bringup";

esp_err_t board_bringup_run(void)
{
    ESP_LOGI(TAG, "==== Basic peripheral self-check ====");
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board_i2c_init failed");
    board_i2c_scan();

    if (CONFIG_INK_RUN_EINK_BRINGUP_ON_BOOT) {
        ESP_RETURN_ON_ERROR(eink_bringup_run(), TAG, "e-paper bring-up failed");
    } else {
        ESP_LOGI(TAG, "E-ink bring-up draw skipped by config");
    }

    ESP_LOGI(TAG, "GPIO / I2C / SPI init sequence executed");
    return ESP_OK;
}

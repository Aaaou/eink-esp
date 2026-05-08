#include "board_bringup.h"

#include "audio_bringup.h"
#include "board_config.h"
#include "board_i2c.h"
#include "eink_bringup.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "project_defaults.h"

static const char *TAG = "board_bringup";

esp_err_t board_bringup_run(void)
{
    board_probe_result_t probe = { 0 };

    ESP_LOGI(TAG, "==== Basic peripheral self-check ====");
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board_i2c_init failed");
    board_i2c_scan();

    ESP_RETURN_ON_ERROR(audio_bringup_run(&probe), TAG, "audio bring-up failed");

    if (CONFIG_INK_RUN_EINK_BRINGUP_ON_BOOT) {
        ESP_RETURN_ON_ERROR(eink_bringup_run(), TAG, "e-paper bring-up failed");
    } else {
        ESP_LOGI(TAG, "[√] E-ink bring-up draw skipped by config");
    }

    if (probe.es8311_found) {
        ESP_LOGI(TAG, "[√] ES8311 address: 0x%02X", probe.es8311_addr);
    } else {
        ESP_LOGW(TAG, "[!] ES8311 not detected; please keep checking address and power");
    }

    ESP_LOGI(TAG, "[√] GPIO / I2C / SPI init sequence executed");
    return ESP_OK;
}

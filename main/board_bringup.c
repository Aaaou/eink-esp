#include "board_bringup.h"

#include "audio_bringup.h"
#include "board_config.h"
#include "board_i2c.h"
#include "eink_bringup.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_bringup";

esp_err_t board_bringup_run(void)
{
    board_probe_result_t probe = { 0 };

    ESP_LOGI(TAG, "Initializing shared I2C bus");
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board_i2c_init failed");
    board_i2c_scan();

    ESP_LOGI(TAG, "Initializing audio path");
    ESP_RETURN_ON_ERROR(audio_bringup_run(&probe), TAG, "audio bring-up failed");

    ESP_LOGI(TAG, "Initializing e-paper path");
    ESP_RETURN_ON_ERROR(eink_bringup_run(), TAG, "e-paper bring-up failed");

    if (probe.es8311_found) {
        ESP_LOGI(TAG, "Summary: ES8311 responded on 0x%02X and panel control path is configured", probe.es8311_addr);
    } else {
        ESP_LOGW(TAG, "Summary: panel control path is configured, but ES8311 address still needs hardware confirmation");
    }

    return ESP_OK;
}

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

    ESP_LOGI(TAG, "==== 基础外设自检 ====");
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board_i2c_init failed");
    board_i2c_scan();

    ESP_RETURN_ON_ERROR(audio_bringup_run(&probe), TAG, "audio bring-up failed");
    ESP_RETURN_ON_ERROR(eink_bringup_run(), TAG, "e-paper bring-up failed");

    if (probe.es8311_found) {
        ESP_LOGI(TAG, "[√] ES8311 地址: 0x%02X", probe.es8311_addr);
    } else {
        ESP_LOGW(TAG, "[!] 未检测到 ES8311，请继续核对硬件地址和供电");
    }

    ESP_LOGI(TAG, "[√] GPIO / I2C / SPI 初始化流程已执行");
    return ESP_OK;
}

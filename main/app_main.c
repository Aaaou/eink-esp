#include "board_bringup.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "===== 墨水屏联调程序启动 =====");

    esp_err_t err = board_bringup_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[!] 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "[√] 初始化完成，请结合屏幕现象继续判断");
}

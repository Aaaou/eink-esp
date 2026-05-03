#include "board_bringup.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "ink_esp bring-up starting");
    esp_err_t err = board_bringup_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bring-up failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Bring-up completed. Ready for hardware validation.");
}

#include "storage_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "storage_service";

esp_err_t storage_service_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 6,
        .format_if_mount_failed = true,
    };

    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "spiffs mount failed");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info("storage", &total, &used), TAG, "spiffs info failed");

    ESP_LOGI(TAG, "[OK] SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

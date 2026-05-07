#include "recording_service.h"

#include "audio_bringup.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portal_service.h"
#include "project_defaults.h"
#include "sdkconfig.h"

#include <stdatomic.h>

static const char *TAG = "recording_service";
static TaskHandle_t s_record_task;
static TaskHandle_t s_portal_task;
static atomic_bool s_recording;
static atomic_bool s_portal_busy;
static atomic_bool s_has_recording;
static atomic_size_t s_file_size;
static const char *s_record_path = "/spiffs/record.wav";

static void record_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&s_recording)) {
            continue;
        }

        atomic_store(&s_recording, true);
        size_t bytes_written = 0;
        esp_err_t err = audio_capture_wav_to_file(s_record_path, CONFIG_AUDIO_RECORD_DURATION_MS, &bytes_written);
        if (err == ESP_OK) {
            atomic_store(&s_has_recording, true);
            atomic_store(&s_file_size, bytes_written);
            ESP_LOGI(TAG, "[OK] Recording finished: %u ms saved to %s",
                CONFIG_AUDIO_RECORD_DURATION_MS,
                s_record_path);
        } else {
            ESP_LOGE(TAG, "[!] Recording failed: %s", esp_err_to_name(err));
        }
        atomic_store(&s_recording, false);
    }
}

static void portal_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "[PORTAL] toggling audio portal from BOOT long press");
        esp_err_t err;
#if CONFIG_AUDIO_PORTAL_DIAGNOSTIC_WIFI_ONLY
        err = portal_service_is_running() ? portal_service_stop() : portal_service_start_minimal_wifi();
#else
        err = portal_service_toggle();
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[PORTAL] toggle failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "[PORTAL] audio portal toggle complete");
        }
        atomic_store(&s_portal_busy, false);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    bool last_level = true;
    TickType_t press_start = 0;
    bool long_press_fired = false;

    while (true) {
        const bool level = gpio_get_level(BOARD_BOOT_BUTTON_GPIO);
        const TickType_t now = xTaskGetTickCount();

        if (last_level && !level) {
            press_start = now;
            long_press_fired = false;
            ESP_LOGI(TAG, "[KEY] BOOT press down");
        } else if (!last_level && !level) {
            const uint32_t held_ms = (uint32_t)pdTICKS_TO_MS(now - press_start);
            if (!long_press_fired && (held_ms >= CONFIG_AUDIO_PORTAL_BUTTON_LONG_PRESS_MS)) {
                if ((s_portal_task == NULL) || atomic_load(&s_portal_busy)) {
                    ESP_LOGI(TAG, "[KEY] BOOT long press ignored: portal busy");
                } else {
                    ESP_LOGI(TAG, "[KEY] BOOT long press detected, queue portal toggle");
                    atomic_store(&s_portal_busy, true);
                    xTaskNotifyGive(s_portal_task);
                }
                long_press_fired = true;
            }
        } else if (!last_level && level) {
            const uint32_t held_ms = (uint32_t)pdTICKS_TO_MS(now - press_start);
            ESP_LOGI(TAG, "[KEY] BOOT release after %lu ms", (unsigned long)held_ms);
            if (!long_press_fired && (held_ms < CONFIG_AUDIO_PORTAL_BUTTON_LONG_PRESS_MS)) {
                ESP_LOGI(TAG, "[KEY] BOOT short press detected, starting local recording");
                esp_err_t err = recording_service_trigger();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "[!] Recording request ignored: %s", esp_err_to_name(err));
                }
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t recording_service_init(void)
{
    const gpio_config_t button_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&button_cfg), TAG, "boot button gpio init failed");
    xTaskCreate(record_task, "record_task", 6144, NULL, 5, &s_record_task);
    xTaskCreate(portal_task, "portal_start_task", 6144, NULL, 4, &s_portal_task);
    xTaskCreate(button_task, "record_btn_task", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "[OK] Recording service ready: button GPIO=%d short_press=record long_press=%dms start_ap duration=%dms",
        BOARD_BOOT_BUTTON_GPIO,
        CONFIG_AUDIO_PORTAL_BUTTON_LONG_PRESS_MS,
        CONFIG_AUDIO_RECORD_DURATION_MS);
    return ESP_OK;
}

esp_err_t recording_service_trigger(void)
{
    ESP_RETURN_ON_FALSE(s_record_task != NULL, ESP_ERR_INVALID_STATE, TAG, "record task not started");
    ESP_RETURN_ON_FALSE(audio_capture_is_ready(), ESP_ERR_INVALID_STATE, TAG, "audio capture unavailable");
    if (atomic_load(&s_recording)) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotifyGive(s_record_task);
    return ESP_OK;
}

void recording_service_get_status(recording_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status->recording = atomic_load(&s_recording);
    status->has_recording = atomic_load(&s_has_recording);
    status->file_size = atomic_load(&s_file_size);
    status->duration_ms = CONFIG_AUDIO_RECORD_DURATION_MS;
    status->sample_rate_hz = CONFIG_AUDIO_RECORD_SAMPLE_RATE;
}

const char *recording_service_get_file_path(void)
{
    return s_record_path;
}

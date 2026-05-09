#include "recording_service.h"

#include "audio_bringup.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portal_service.h"
#include "sdkconfig.h"
#include "project_defaults.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "recording_service";

#define RECORDING_EXPECTED_FILE_SIZE \
    ((size_t)44U + ((size_t)CONFIG_AUDIO_RECORD_SAMPLE_RATE * 2U * (size_t)CONFIG_AUDIO_RECORD_DURATION_MS / 1000U))

static TaskHandle_t s_record_task;
static TaskHandle_t s_play_task;
static TaskHandle_t s_portal_task;
static atomic_bool s_recording;
static atomic_bool s_record_request_pending;
static atomic_bool s_play_request_pending;
static atomic_bool s_portal_busy;
static atomic_bool s_has_recording;
static atomic_size_t s_file_size;
static atomic_uint s_record_id;
static char s_record_path[40] = "/spiffs/record.wav";
static char s_download_name[32] = "record.wav";
static char s_play_path[40];

static void recording_copy_str(char *dst, const char *src, size_t dst_size)
{
    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static uint16_t recording_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t recording_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static bool recording_wav_file_is_valid(const char *path, size_t *file_size)
{
    uint8_t header[44];
    struct stat st;
    FILE *file;
    uint32_t data_size;

    if ((path == NULL) || (stat(path, &st) != 0) || (st.st_size < (off_t)sizeof(header))) {
        return false;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    const size_t got = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (got != sizeof(header)) {
        return false;
    }

    if ((memcmp(&header[0], "RIFF", 4) != 0) ||
        (memcmp(&header[8], "WAVE", 4) != 0) ||
        (memcmp(&header[12], "fmt ", 4) != 0) ||
        (memcmp(&header[36], "data", 4) != 0)) {
        return false;
    }
    if ((recording_read_le16(&header[20]) != 1U) ||
        (recording_read_le16(&header[22]) != 1U) ||
        (recording_read_le32(&header[24]) != CONFIG_AUDIO_RECORD_SAMPLE_RATE) ||
        (recording_read_le16(&header[34]) != 16U)) {
        return false;
    }

    data_size = recording_read_le32(&header[40]);
    if ((data_size == 0U) || ((uint32_t)st.st_size < (uint32_t)sizeof(header) + data_size)) {
        return false;
    }

    if (file_size != NULL) {
        *file_size = (size_t)st.st_size;
    }
    return true;
}

static void recording_refresh_file_status(void)
{
    size_t file_size = 0;

    if (atomic_load(&s_recording) || atomic_load(&s_record_request_pending) || atomic_load(&s_play_request_pending)) {
        return;
    }

    if (recording_wav_file_is_valid(s_record_path, &file_size)) {
        atomic_store(&s_has_recording, true);
        atomic_store(&s_file_size, file_size);
    } else {
        if (remove(s_record_path) == 0) {
            ESP_LOGW(TAG, "[REC] Removed invalid recording file: %s", s_record_path);
        }
        atomic_store(&s_has_recording, false);
        atomic_store(&s_file_size, 0);
    }
}

static void recording_remove_old_files(void)
{
    remove("/spiffs/record.wav");
    remove("/spiffs/record_slot_1.wav");
    remove("/spiffs/record_slot_2.wav");
    remove("/spiffs/record_slot_3.wav");
    remove("/spiffs/rec_000001.wav");
    remove("/spiffs/rec_000002.wav");
    remove("/spiffs/rec_000003.wav");
    remove("/spiffs/rec_000004.wav");
    remove("/spiffs/rec_000005.wav");
}

static void record_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        atomic_store(&s_record_request_pending, false);
        if (atomic_load(&s_recording)) {
            continue;
        }

        atomic_store(&s_recording, true);
        if (atomic_load(&s_has_recording)) {
            const uint32_t record_id = atomic_load(&s_record_id);
            ESP_LOGI(TAG, "[LOOP %06lu] playback start path=%s", (unsigned long)record_id, s_record_path);
            esp_err_t err = audio_play_wav_file(s_record_path);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "[LOOP %06lu] playback done; next short press records again", (unsigned long)record_id);
                atomic_store(&s_has_recording, false);
                atomic_store(&s_file_size, 0);
            } else {
                ESP_LOGE(TAG, "[LOOP %06lu] playback failed: %s", (unsigned long)record_id, esp_err_to_name(err));
                recording_refresh_file_status();
            }
            atomic_store(&s_recording, false);
            continue;
        }

        const uint32_t record_id = atomic_fetch_add(&s_record_id, 1U) + 1U;
        size_t bytes_written = 0;
        const int64_t start_ms = esp_log_timestamp();

        recording_remove_old_files();
        recording_copy_str(s_record_path, "/spiffs/record.wav", sizeof(s_record_path));
        recording_copy_str(s_download_name, "record.wav", sizeof(s_download_name));

        ESP_LOGI(TAG,
            "[LOOP %06lu] record start duration=%u ms sample_rate=%u expected_file=%u target=FLASH path=%s",
            (unsigned long)record_id,
            CONFIG_AUDIO_RECORD_DURATION_MS,
            CONFIG_AUDIO_RECORD_SAMPLE_RATE,
            (unsigned)RECORDING_EXPECTED_FILE_SIZE,
            s_record_path);

        esp_err_t err = audio_capture_wav_to_file(s_record_path, CONFIG_AUDIO_RECORD_DURATION_MS, &bytes_written);
        if (err == ESP_OK) {
            const int64_t elapsed_ms = (int64_t)esp_log_timestamp() - start_ms;
            atomic_store(&s_has_recording, true);
            atomic_store(&s_file_size, bytes_written);
            ESP_LOGI(TAG,
                "[LOOP %06lu] record done elapsed=%lld ms file_size=%u expected_file=%u",
                (unsigned long)record_id,
                (long long)elapsed_ms,
                (unsigned)bytes_written,
                (unsigned)RECORDING_EXPECTED_FILE_SIZE);
            if (bytes_written != RECORDING_EXPECTED_FILE_SIZE) {
                ESP_LOGW(TAG,
                    "[LOOP %06lu] WAV size mismatch: bytes_written=%u expected=%u",
                    (unsigned long)record_id,
                    (unsigned)bytes_written,
                    (unsigned)RECORDING_EXPECTED_FILE_SIZE);
            }
            ESP_LOGI(TAG, "[LOOP %06lu] flash recording ready; next short press plays it", (unsigned long)record_id);
        } else {
            ESP_LOGE(TAG, "[LOOP %06lu] record failed: %s", (unsigned long)record_id, esp_err_to_name(err));
            atomic_store(&s_has_recording, false);
            atomic_store(&s_file_size, 0);
        }
        atomic_store(&s_recording, false);
    }
}

static void play_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        atomic_store(&s_play_request_pending, false);
        if (atomic_load(&s_recording)) {
            ESP_LOGW(TAG, "[PLAY] ignored: recording is active");
            continue;
        }

        atomic_store(&s_recording, true);
        ESP_LOGI(TAG, "[PLAY] web playback start path=%s", s_play_path);
        esp_err_t err = audio_play_wav_file(s_play_path);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[PLAY] web playback done path=%s", s_play_path);
        } else {
            ESP_LOGE(TAG, "[PLAY] web playback failed: %s", esp_err_to_name(err));
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
                recording_refresh_file_status();
                ESP_LOGI(TAG,
                    "[KEY] BOOT short press detected, queue local %s",
                    atomic_load(&s_has_recording) ? "playback" : "recording");
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
    recording_refresh_file_status();
    xTaskCreate(record_task, "record_task", 6144, NULL, 5, &s_record_task);
    xTaskCreate(play_task, "record_play_task", 6144, NULL, 5, &s_play_task);
    xTaskCreate(portal_task, "portal_start_task", 6144, NULL, 4, &s_portal_task);
    xTaskCreate(button_task, "record_btn_task", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "[OK] Recording service ready: button GPIO=%d short_press=record/play flash long_press=%dms start_ap duration=%dms",
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
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_record_request_pending, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotifyGive(s_record_task);
    return ESP_OK;
}

esp_err_t recording_service_play_file(const char *path)
{
    struct stat st;
    bool expected = false;

    ESP_RETURN_ON_FALSE(s_play_task != NULL, ESP_ERR_INVALID_STATE, TAG, "play task not started");
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "play path null");
    ESP_RETURN_ON_FALSE(strncmp(path, "/spiffs/", 8) == 0, ESP_ERR_INVALID_ARG, TAG, "play path outside spiffs");
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0, ESP_ERR_NOT_FOUND, TAG, "play file missing");
    if (atomic_load(&s_recording)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_compare_exchange_strong(&s_play_request_pending, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    recording_copy_str(s_play_path, path, sizeof(s_play_path));
    xTaskNotifyGive(s_play_task);
    return ESP_OK;
}

esp_err_t recording_service_delete_recording(void)
{
    if (atomic_load(&s_recording) || atomic_load(&s_record_request_pending) || atomic_load(&s_play_request_pending)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (remove(s_record_path) != 0) {
        recording_refresh_file_status();
        return atomic_load(&s_has_recording) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }

    atomic_store(&s_has_recording, false);
    atomic_store(&s_file_size, 0);
    ESP_LOGI(TAG, "[REC] Deleted recording file: %s", s_record_path);
    return ESP_OK;
}

void recording_service_get_status(recording_status_t *status)
{
    if (status == NULL) {
        return;
    }
    recording_refresh_file_status();
    status->recording = atomic_load(&s_recording);
    status->has_recording = atomic_load(&s_has_recording);
    status->file_size = atomic_load(&s_file_size);
    status->expected_file_size = RECORDING_EXPECTED_FILE_SIZE;
    status->record_id = atomic_load(&s_record_id);
    status->active_record_id = status->recording ? status->record_id : 0;
    status->duration_ms = CONFIG_AUDIO_RECORD_DURATION_MS;
    status->sample_rate_hz = CONFIG_AUDIO_RECORD_SAMPLE_RATE;
    recording_copy_str(status->path, s_record_path, sizeof(status->path));
    recording_copy_str(status->download_name, s_download_name, sizeof(status->download_name));
}

const char *recording_service_get_file_path(void)
{
    return s_record_path;
}

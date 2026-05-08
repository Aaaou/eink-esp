#include "serial_export_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "recording_service.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "serial_export";

#define SERIAL_CMD_BUF_SIZE 64
#define SERIAL_RAW_CHUNK_SIZE 384
#define SERIAL_B64_CHUNK_SIZE ((((SERIAL_RAW_CHUNK_SIZE) + 2) / 3) * 4 + 1)

static void print_help(void)
{
    printf("\r\n[serial] commands:\r\n");
    printf("[serial]   help   - show this message\r\n");
    printf("[serial]   status - show recording status\r\n");
    printf("[serial]   export - export /spiffs/record.wav as base64 chunks\r\n");
}

static void print_status(void)
{
    recording_status_t status = { 0 };
    recording_service_get_status(&status);
    printf("[serial] recording=%s has_recording=%s completed_id=%u active_id=%u file_size=%u expected=%u duration_ms=%u sample_rate=%u path=%s name=%s\r\n",
        status.recording ? "true" : "false",
        status.has_recording ? "true" : "false",
        (unsigned)status.record_id,
        (unsigned)status.active_record_id,
        (unsigned)status.file_size,
        (unsigned)status.expected_file_size,
        (unsigned)status.duration_ms,
        (unsigned)status.sample_rate_hz,
        status.path,
        status.download_name);
}

esp_err_t serial_export_service_export_recording(void)
{
    const char *path = recording_service_get_file_path();
    recording_status_t status = { 0 };
    struct stat st;
    recording_service_get_status(&status);
    if (stat(path, &st) != 0) {
        printf("[serial] export failed: recording file not found at %s\r\n", path);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("[serial] export failed: unable to open %s\r\n", path);
        return ESP_FAIL;
    }

    printf("[serial] export begin completed_id=%u path=%s name=%s size=%u expected=%u encoding=base64\r\n",
        (unsigned)status.record_id,
        path,
        status.download_name,
        (unsigned)st.st_size,
        (unsigned)status.expected_file_size);

    unsigned char raw[SERIAL_RAW_CHUNK_SIZE];
    unsigned char b64[SERIAL_B64_CHUNK_SIZE];
    size_t chunk_index = 0;

    while (true) {
        size_t bytes_read = fread(raw, 1, sizeof(raw), file);
        if (bytes_read == 0) {
            break;
        }

        size_t out_len = 0;
        int rc = mbedtls_base64_encode(b64, sizeof(b64), &out_len, raw, bytes_read);
        if (rc != 0) {
            printf("[serial] export failed: base64 encode error %d\r\n", rc);
            fclose(file);
            return ESP_FAIL;
        }

        b64[out_len] = '\0';
        printf("[serial] chunk %u %s\r\n", (unsigned)chunk_index, (const char *)b64);
        ++chunk_index;
    }

    fclose(file);
    printf("[serial] export end chunks=%u\r\n", (unsigned)chunk_index);
    return ESP_OK;
}

static void handle_command(const char *cmd)
{
    if ((strcmp(cmd, "help") == 0) || (strcmp(cmd, "?") == 0)) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "export") == 0) {
        serial_export_service_export_recording();
    } else if (cmd[0] != '\0') {
        printf("[serial] unknown command: %s\r\n", cmd);
        print_help();
    }
}

static void serial_console_task(void *arg)
{
    (void)arg;

    char cmd_buf[SERIAL_CMD_BUF_SIZE];
    size_t len = 0;

    print_help();
    printf("[serial] ready\r\n");

    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if ((ch == '\r') || (ch == '\n')) {
            if (len > 0) {
                cmd_buf[len] = '\0';
                handle_command(cmd_buf);
                len = 0;
            }
            continue;
        }

        if ((ch == '\b') || (ch == 127)) {
            if (len > 0) {
                --len;
            }
            continue;
        }

        if ((len + 1) < sizeof(cmd_buf)) {
            cmd_buf[len++] = (char)ch;
        }
    }
}

esp_err_t serial_export_service_init(void)
{
    BaseType_t ok = xTaskCreate(serial_console_task, "serial_console", 4096, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "serial console task create failed");
    ESP_LOGI(TAG, "[OK] Serial export service ready; type 'help', 'status', or 'export'");
    return ESP_OK;
}

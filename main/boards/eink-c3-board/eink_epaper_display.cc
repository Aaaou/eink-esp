#include "eink_epaper_display.h"

#include "config.h"
#include "pcf8574_device.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "EinkEpaperDisplay";

EinkEpaperDisplay::EinkEpaperDisplay(int width, int height, Pcf8574Device* iox) : iox_(iox) {
    width_ = width;
    height_ = height;
    InitializePanelPower();
    HardwareReset();
}

EinkEpaperDisplay::~EinkEpaperDisplay() {
}

void EinkEpaperDisplay::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus: %s", status ? status : "");
}

void EinkEpaperDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "ShowNotification(%d ms): %s", duration_ms, notification ? notification : "");
}

void EinkEpaperDisplay::SetChatMessage(const char* role, const char* content) {
    ESP_LOGI(TAG, "SetChatMessage role=%s content=%s", role ? role : "", content ? content : "");
}

void EinkEpaperDisplay::ClearChatMessages() {
    ESP_LOGI(TAG, "ClearChatMessages");
}

void EinkEpaperDisplay::SetPowerSaveMode(bool on) {
    power_save_mode_ = on;
    ESP_LOGI(TAG, "SetPowerSaveMode: %d", on);
}

void EinkEpaperDisplay::SetupUI() {
    Display::SetupUI();
    ESP_LOGI(TAG, "SetupUI");
}

void EinkEpaperDisplay::InitializePanelPower() {
    if (iox_ == nullptr) {
        ESP_LOGW(TAG, "PCF8574 is null, skip panel power init");
        return;
    }
    iox_->SetBit(BOARD_IOX_BIT_EINK_CTRL_4150B, true);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_POWER_SETTLE_MS));
}

void EinkEpaperDisplay::HardwareReset() {
    if (iox_ == nullptr) {
        ESP_LOGW(TAG, "PCF8574 is null, skip panel reset");
        return;
    }
    iox_->SetBit(BOARD_IOX_BIT_EINK_RES, false);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_RESET_HOLD_MS));
    iox_->SetBit(BOARD_IOX_BIT_EINK_RES, true);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_RESET_RELEASE_MS));
}

bool EinkEpaperDisplay::Lock(int timeout_ms) {
    (void)timeout_ms;
    return true;
}

void EinkEpaperDisplay::Unlock() {
}

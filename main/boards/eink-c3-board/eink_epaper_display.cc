#include "eink_epaper_display.h"

#include "config.h"
#include "pcf8574_device.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "EinkEpaperDisplay";

static constexpr uint32_t kPanelPowerOnTimeMs = 100;
static constexpr uint32_t kPanelPowerOffTimeMs = 50;
static constexpr uint32_t kPanelFullRefreshTimeMs = 15000;

struct AsciiGlyph {
    char ch;
    uint8_t rows[7];
};

static const AsciiGlyph kAsciiFont[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

static const AsciiGlyph& FindAsciiGlyph(char ch) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (const auto& glyph : kAsciiFont) {
        if (glyph.ch == upper) {
            return glyph;
        }
    }
    return kAsciiFont[0];
}

EinkEpaperDisplay::EinkEpaperDisplay(int width, int height, Pcf8574Device* iox) : iox_(iox) {
    width_ = width;
    height_ = height;
    ClearFrame();
    previous_frame_.fill(0xFF);
    auto ret = InitializeSpi();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(ret));
    }
    StartRefreshTask();
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

void EinkEpaperDisplay::ShowRunningStatusWithDate() {
    time_t now = time(nullptr);
    struct tm timeinfo = {};
    localtime_r(&now, &timeinfo);

    char date[32];
    if (timeinfo.tm_year >= 100) {
        int year = timeinfo.tm_year + 1900;
        int month = std::max(1, std::min(12, timeinfo.tm_mon + 1));
        int day = std::max(1, std::min(31, timeinfo.tm_mday));
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d",
            year,
            month,
            day);
    } else {
        std::snprintf(date, sizeof(date), "DATE READY");
    }
    QueuePresent("XIAOZHI RUN", date);
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
    vTaskDelay(pdMS_TO_TICKS(200));
}

esp_err_t EinkEpaperDisplay::BeginSession() {
    ESP_RETURN_ON_ERROR(InitializeSpi(), TAG, "SPI init failed");
    InitializePanelPower();
    HardwareReset();
    panel_powered_ = false;
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::InitializeSpi() {
    if (spi_initialized_) {
        return ESP_OK;
    }

    gpio_config_t dc_cfg = {};
    dc_cfg.pin_bit_mask = 1ULL << DISPLAY_DC_PIN;
    dc_cfg.mode = GPIO_MODE_OUTPUT;
    dc_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    dc_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dc_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&dc_cfg), TAG, "DC gpio init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_DC_PIN, 0), TAG, "DC init level failed");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = 256;

    esp_err_t ret = spi_bus_initialize(DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = DISPLAY_SPI_CLOCK_HZ;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = DISPLAY_SPI_CS_PIN;
    dev_cfg.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(DISPLAY_SPI_HOST, &dev_cfg, &spi_), TAG, "add spi device failed");
    spi_initialized_ = true;
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::SendCommand(uint8_t command) {
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &command;
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_DC_PIN, 0), TAG, "DC command failed");
    return spi_device_transmit(spi_, &t);
}

esp_err_t EinkEpaperDisplay::SendData(const uint8_t* data, size_t len) {
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_DC_PIN, 1), TAG, "DC data failed");
    return spi_device_transmit(spi_, &t);
}

esp_err_t EinkEpaperDisplay::SendDataByte(uint8_t data) {
    return SendData(&data, 1);
}

esp_err_t EinkEpaperDisplay::SendRepeat(uint8_t value, size_t len) {
    uint8_t chunk[32];
    std::memset(chunk, value, sizeof(chunk));
    while (len > 0) {
        size_t now = std::min(len, sizeof(chunk));
        ESP_RETURN_ON_ERROR(SendData(chunk, now), TAG, "repeat data failed");
        len -= now;
    }
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::PowerOn() {
    if (!panel_powered_) {
        ESP_RETURN_ON_ERROR(SendCommand(0x04), TAG, "power on command failed");
        vTaskDelay(pdMS_TO_TICKS(kPanelPowerOnTimeMs));
        panel_powered_ = true;
    }
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::PowerOff() {
    if (panel_powered_) {
        ESP_RETURN_ON_ERROR(SendCommand(0x02), TAG, "power off command failed");
        vTaskDelay(pdMS_TO_TICKS(kPanelPowerOffTimeMs));
        panel_powered_ = false;
    }
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::SleepPanel() {
    ESP_RETURN_ON_ERROR(PowerOff(), TAG, "power off before sleep failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x07), TAG, "sleep command failed");
    ESP_RETURN_ON_ERROR(SendDataByte(0xA5), TAG, "sleep data failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    // Keep BOARD_IOX_BIT_EINK_CTRL_4150B high. On this board it also gates the audio amplifier path.
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::InitPanel() {
    const uint8_t resolution[] = {
        static_cast<uint8_t>(DISPLAY_WIDTH),
        static_cast<uint8_t>(DISPLAY_HEIGHT >> 8),
        static_cast<uint8_t>(DISPLAY_HEIGHT & 0xFF),
    };
    ESP_RETURN_ON_ERROR(SendCommand(0x00), TAG, "cmd 0x00 failed");
    ESP_RETURN_ON_ERROR(SendDataByte(0x8F), TAG, "data 0x8F failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x61), TAG, "cmd 0x61 failed");
    ESP_RETURN_ON_ERROR(SendData(resolution, sizeof(resolution)), TAG, "resolution failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x50), TAG, "cmd 0x50 failed");
    ESP_RETURN_ON_ERROR(SendDataByte(0x77), TAG, "data 0x77 failed");
    return PowerOn();
}

esp_err_t EinkEpaperDisplay::SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t xe = static_cast<uint16_t>((x + w - 1U) | 0x0007U);
    uint16_t ye = static_cast<uint16_t>(y + h - 1U);
    x &= 0xFFF8U;

    ESP_RETURN_ON_ERROR(SendCommand(0x90), TAG, "cmd 0x90 failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(x & 0xFF)), TAG, "x failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(xe & 0xFF)), TAG, "xe failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(y >> 8)), TAG, "y hi failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(y & 0xFF)), TAG, "y lo failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(ye >> 8)), TAG, "ye hi failed");
    ESP_RETURN_ON_ERROR(SendDataByte(static_cast<uint8_t>(ye & 0xFF)), TAG, "ye lo failed");
    ESP_RETURN_ON_ERROR(SendDataByte(0x01), TAG, "area tail failed");
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::WritePlane(uint8_t command, const uint8_t* bitmap) {
    ESP_RETURN_ON_ERROR(SendCommand(command), TAG, "plane command failed");
    return SendData(bitmap, kBufferSize);
}

esp_err_t EinkEpaperDisplay::WriteFullPlane(uint8_t command, const uint8_t* bitmap) {
    ESP_RETURN_ON_ERROR(InitPanel(), TAG, "write plane init failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x91), TAG, "partial in failed");
    ESP_RETURN_ON_ERROR(SetPartialRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT), TAG, "area failed");
    ESP_RETURN_ON_ERROR(WritePlane(command, bitmap), TAG, "plane data failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x92), TAG, "partial out failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::WriteFullPlaneRepeat(uint8_t command, uint8_t value) {
    ESP_RETURN_ON_ERROR(InitPanel(), TAG, "write repeat plane init failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x91), TAG, "partial in failed");
    ESP_RETURN_ON_ERROR(SetPartialRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT), TAG, "area failed");
    ESP_RETURN_ON_ERROR(SendCommand(command), TAG, "plane command failed");
    ESP_RETURN_ON_ERROR(SendRepeat(value, kBufferSize), TAG, "plane repeat failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x92), TAG, "partial out failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

esp_err_t EinkEpaperDisplay::RefreshFrame() {
    ESP_RETURN_ON_ERROR(BeginSession(), TAG, "begin session failed");

    if (first_frame_) {
        ESP_RETURN_ON_ERROR(WriteFullPlaneRepeat(0x10, 0xFF), TAG, "initial old plane failed");
        ESP_RETURN_ON_ERROR(WriteFullPlaneRepeat(0x13, 0xFF), TAG, "initial new plane failed");
        first_frame_ = false;
    }

    ESP_RETURN_ON_ERROR(WriteFullPlane(0x10, frame_.data()), TAG, "black plane failed");
    ESP_RETURN_ON_ERROR(WriteFullPlaneRepeat(0x13, 0xFF), TAG, "color plane failed");
    ESP_RETURN_ON_ERROR(SendCommand(0x12), TAG, "refresh failed");
    vTaskDelay(pdMS_TO_TICKS(kPanelFullRefreshTimeMs));
    previous_frame_ = frame_;
    return SleepPanel();
}

void EinkEpaperDisplay::ClearFrame() {
    frame_.fill(0xFF);
}

void EinkEpaperDisplay::SetPixel(int x, int y, bool black) {
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    const int stride = DISPLAY_WIDTH / 8;
    size_t index = static_cast<size_t>(y * stride + x / 8);
    uint8_t mask = static_cast<uint8_t>(0x80U >> (x % 8));
    if (black) {
        frame_[index] &= static_cast<uint8_t>(~mask);
    } else {
        frame_[index] |= mask;
    }
}

void EinkEpaperDisplay::SetLandscapePixel(int x, int y, bool black) {
    if (x < 0 || x >= DISPLAY_HEIGHT || y < 0 || y >= DISPLAY_WIDTH) {
        return;
    }
    SetPixel(y, DISPLAY_HEIGHT - 1 - x, black);
}

void EinkEpaperDisplay::DrawAsciiChar(int x, int y, char c, int scale) {
    const auto& glyph = FindAsciiGlyph(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph.rows[row] >> (4 - col)) & 0x01) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        SetPixel(x + col * scale + sx, y + row * scale + sy, true);
                    }
                }
            }
        }
    }
}

void EinkEpaperDisplay::DrawLandscapeAsciiChar(int x, int y, char c, int scale) {
    const auto& glyph = FindAsciiGlyph(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph.rows[row] >> (4 - col)) & 0x01) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        SetLandscapePixel(x + col * scale + sx, y + row * scale + sy, true);
                    }
                }
            }
        }
    }
}

void EinkEpaperDisplay::DrawLandscapeAsciiText(int x, int y, const char* text, int scale) {
    int cursor_x = x;
    for (size_t i = 0; text != nullptr && text[i] != '\0'; ++i) {
        DrawLandscapeAsciiChar(cursor_x, y, text[i], scale);
        cursor_x += 6 * scale;
    }
}

void EinkEpaperDisplay::DrawLandscapeCenteredAscii(int center_x, int y, const char* text, int scale) {
    int len = text ? std::strlen(text) : 0;
    int width = len > 0 ? (len * 5 * scale) + ((len - 1) * scale) : 0;
    DrawLandscapeAsciiText(center_x - width / 2, y, text, scale);
}

void EinkEpaperDisplay::DrawLandscapeHLine(int x, int y, int w) {
    for (int ix = x; ix < x + w; ++ix) {
        SetLandscapePixel(ix, y, true);
    }
}

void EinkEpaperDisplay::DrawLandscapeVLine(int x, int y, int h) {
    for (int iy = y; iy < y + h; ++iy) {
        SetLandscapePixel(x, iy, true);
    }
}

void EinkEpaperDisplay::DrawLandscapeRect(int x, int y, int w, int h) {
    DrawLandscapeHLine(x, y, w);
    DrawLandscapeHLine(x, y + h - 1, w);
    DrawLandscapeVLine(x, y, h);
    DrawLandscapeVLine(x + w - 1, y, h);
}

void EinkEpaperDisplay::FillLandscapeRect(int x, int y, int w, int h) {
    for (int iy = y; iy < y + h; ++iy) {
        DrawLandscapeHLine(x, iy, w);
    }
}

void EinkEpaperDisplay::DrawAsciiText(int x, int y, const char* text, int scale) {
    int cursor_x = x;
    for (size_t i = 0; text != nullptr && text[i] != '\0'; ++i) {
        DrawAsciiChar(cursor_x, y, text[i], scale);
        cursor_x += 6 * scale;
    }
}

void EinkEpaperDisplay::DrawCenteredAscii(int center_x, int y, const char* text, int scale) {
    int len = text ? std::strlen(text) : 0;
    int width = len > 0 ? (len * 5 * scale) + ((len - 1) * scale) : 0;
    DrawAsciiText(center_x - width / 2, y, text, scale);
}

void EinkEpaperDisplay::RenderMessage(const char* status, const char* content) {
    ClearFrame();

    DrawLandscapeRect(2, 2, DISPLAY_HEIGHT - 4, DISPLAY_WIDTH - 4);
    FillLandscapeRect(10, 12, 192, 14);
    DrawLandscapeCenteredAscii(106, 36, status && status[0] ? status : "XIAOZHI RUN", 2);
    DrawLandscapeHLine(18, 62, 176);
    DrawLandscapeCenteredAscii(106, 72, content && content[0] ? content : "DATE READY", 1);
    DrawLandscapeCenteredAscii(106, 88, BOARD_NAME, 1);

    size_t black_pixels = 0;
    for (uint8_t byte : frame_) {
        black_pixels += 8U - __builtin_popcount(byte);
    }
    ESP_LOGI(TAG, "RenderMessage black_pixels=%u", static_cast<unsigned>(black_pixels));
}

void EinkEpaperDisplay::QueuePresent(const char* status, const char* content) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status != nullptr) {
            pending_status_ = status;
        }
        if (content != nullptr) {
            pending_content_ = content;
        }
    }
    if (refresh_task_ != nullptr) {
        xTaskNotifyGive(refresh_task_);
    }
}

void EinkEpaperDisplay::RefreshTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        std::string status;
        std::string content;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status = pending_status_;
            content = pending_content_;
        }

        RenderMessage(status.c_str(), content.c_str());
        auto ret = RefreshFrame();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Refresh failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Refresh done");
        }
    }
}

void EinkEpaperDisplay::StartRefreshTask() {
    if (refresh_task_ != nullptr) {
        return;
    }
    xTaskCreate([](void* arg) {
        auto* display = static_cast<EinkEpaperDisplay*>(arg);
        display->RefreshTask();
    }, "eink_refresh", 4096, this, 2, &refresh_task_);
}

bool EinkEpaperDisplay::Lock(int timeout_ms) {
    (void)timeout_ms;
    mutex_.lock();
    return true;
}

void EinkEpaperDisplay::Unlock() {
    mutex_.unlock();
}

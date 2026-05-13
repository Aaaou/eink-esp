#ifndef _EINK_EPAPER_DISPLAY_H_
#define _EINK_EPAPER_DISPLAY_H_

#include "display.h"

#include <array>
#include <mutex>
#include <string>

#include <driver/spi_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Pcf8574Device;

class EinkEpaperDisplay : public Display {
public:
    EinkEpaperDisplay(int width, int height, Pcf8574Device* iox);
    virtual ~EinkEpaperDisplay();

    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetupUI() override;
    void ShowRunningStatusWithDate();

private:
    static constexpr size_t kBufferSize = 104 * 212 / 8;

    bool power_save_mode_ = false;
    bool spi_initialized_ = false;
    bool panel_powered_ = false;
    bool first_frame_ = true;
    Pcf8574Device* iox_ = nullptr;
    spi_device_handle_t spi_ = nullptr;
    std::mutex mutex_;
    TaskHandle_t refresh_task_ = nullptr;
    std::string pending_status_ = "小智控制中";
    std::string pending_content_ = "小智控制中";
    std::array<uint8_t, kBufferSize> frame_{};
    std::array<uint8_t, kBufferSize> previous_frame_{};

    void InitializePanelPower();
    void HardwareReset();
    esp_err_t BeginSession();
    esp_err_t InitializeSpi();
    esp_err_t SendCommand(uint8_t command);
    esp_err_t SendData(const uint8_t* data, size_t len);
    esp_err_t SendDataByte(uint8_t data);
    esp_err_t SendRepeat(uint8_t value, size_t len);
    esp_err_t PowerOn();
    esp_err_t PowerOff();
    esp_err_t SleepPanel();
    esp_err_t InitPanel();
    esp_err_t SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    esp_err_t WritePlane(uint8_t command, const uint8_t* bitmap);
    esp_err_t WriteFullPlane(uint8_t command, const uint8_t* bitmap);
    esp_err_t WriteFullPlaneRepeat(uint8_t command, uint8_t value);
    esp_err_t RefreshFrame();
    void ClearFrame();
    void SetPixel(int x, int y, bool black);
    void SetLandscapePixel(int x, int y, bool black);
    void DrawCenteredAscii(int center_x, int y, const char* text, int scale);
    void DrawAsciiText(int x, int y, const char* text, int scale);
    void DrawAsciiChar(int x, int y, char c, int scale);
    void DrawLandscapeCenteredAscii(int center_x, int y, const char* text, int scale);
    void DrawLandscapeAsciiText(int x, int y, const char* text, int scale);
    void DrawLandscapeAsciiChar(int x, int y, char c, int scale);
    void DrawLandscapeHLine(int x, int y, int w);
    void DrawLandscapeVLine(int x, int y, int h);
    void DrawLandscapeRect(int x, int y, int w, int h);
    void FillLandscapeRect(int x, int y, int w, int h);
    void RenderMessage(const char* status, const char* content);
    void QueuePresent(const char* status, const char* content);
    void RefreshTask();
    void StartRefreshTask();

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
};

#endif // _EINK_EPAPER_DISPLAY_H_

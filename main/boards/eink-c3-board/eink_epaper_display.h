#ifndef _EINK_EPAPER_DISPLAY_H_
#define _EINK_EPAPER_DISPLAY_H_

#include "display.h"

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

private:
    bool power_save_mode_ = false;
    Pcf8574Device* iox_ = nullptr;

    void InitializePanelPower();
    void HardwareReset();

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
};

#endif // _EINK_EPAPER_DISPLAY_H_

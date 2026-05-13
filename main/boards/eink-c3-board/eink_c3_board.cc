#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "eink_ble_service.h"
#include "eink_epaper_display.h"
#include "eink_mode_manager.h"
#include "eink_scene_legacy.h"
#include "pcf8574_device.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_system.h>

static const char* TAG = "EinkC3Board";

EinkBoardMode GetEinkBoardMode() {
    return EinkModeManager::LoadMode();
}

bool IsEpaperBleBoardMode() {
    return GetEinkBoardMode() == EinkBoardMode::EpaperBle;
}

class EinkC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    EinkEpaperDisplay* display_ = nullptr;
    Button boot_button_;
    Pcf8574Device* iox_ = nullptr;
    EinkBleService* ble_service_ = nullptr;
    EinkBoardMode mode_ = EinkBoardMode::XiaoZhi;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeDisplay() {
        display_ = new EinkEpaperDisplay(DISPLAY_WIDTH, DISPLAY_HEIGHT, iox_);
        eink_scene_bind_display(display_);
    }

    void InitializeIox() {
        iox_ = new Pcf8574Device(codec_i2c_bus_, BOARD_IOX_ADDR, 0xFF);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (mode_ == EinkBoardMode::EpaperBle) {
                display_->SetModeMessage("EPD BLE", "WAIT MINI APP");
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            EinkBoardMode next = EinkModeManager::ToggleMode();
            ESP_LOGW(TAG, "Switch board mode to %s and reboot", next == EinkBoardMode::EpaperBle ? "epaper-ble" : "xiaozhi");
            esp_restart();
        });
    }

    void InitializeMode() {
        mode_ = EinkModeManager::LoadMode();
        ESP_LOGI(TAG, "Current board mode: %s", mode_ == EinkBoardMode::EpaperBle ? "epaper-ble" : "xiaozhi");
    }

    void InitializeBleMode() {
        if (mode_ != EinkBoardMode::EpaperBle) {
            return;
        }
        ble_service_ = new EinkBleService(display_);
        ESP_ERROR_CHECK(ble_service_->Start());
    }

public:
    EinkC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Initialize eink-c3-board");
        InitializeMode();
        InitializeCodecI2c();
        InitializeIox();
        InitializeDisplay();
        InitializeButtons();
        InitializeBleMode();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual void StartNetwork() override {
        if (mode_ == EinkBoardMode::EpaperBle) {
            ESP_LOGI(TAG, "Skip XiaoZhi network startup in epaper BLE mode");
            return;
        }
        WifiBoard::StartNetwork();
    }
};

DECLARE_BOARD(EinkC3Board);

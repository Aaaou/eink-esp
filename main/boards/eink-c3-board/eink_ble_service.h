#ifndef _EINK_BLE_SERVICE_H_
#define _EINK_BLE_SERVICE_H_

#include "esp_err.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class EinkEpaperDisplay;

class EinkBleService {
public:
    static constexpr uint8_t kAdvInstance = 0;

    enum HandleIndex : uint8_t {
        kSvc = 0,
        kRxCharDecl,
        kRxCharValue,
        kTxCharDecl,
        kTxCharValue,
        kTxCharCccd,
        kStateCharDecl,
        kStateCharValue,
        kHandleCount,
    };

    EinkBleService(EinkEpaperDisplay* display);
    esp_err_t Start();
    bool started() const { return started_; }

private:
    static constexpr uint16_t kAppId = 0x45E1;
    static constexpr uint16_t kDeviceNameMaxLen = 31;

    EinkEpaperDisplay* display_ = nullptr;
    bool started_ = false;
    uint16_t gatts_if_ = ESP_GATT_IF_NONE;
    uint16_t conn_id_ = 0;
    bool connected_ = false;
    bool notify_enabled_ = false;
    std::array<uint16_t, kHandleCount> handles_{};
    std::vector<uint8_t> frame_buffer_;

    static EinkBleService* s_instance;

    static void GapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    static void GattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param);

    void HandleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void HandleGattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param);

    esp_err_t InitBleStack();
    esp_err_t RegisterService();
    esp_err_t ConfigureAdvertising();
    void StartAdvertising();
    void HandleWrite(const esp_ble_gatts_cb_param_t::gatts_write_evt_param& write);
    void HandleRead(const esp_ble_gatts_cb_param_t::gatts_read_evt_param& read);
    void NotifyTextResult(uint8_t opcode, uint8_t result_code, const char* message);
    void UpdateDisplayForBleMessage(const char* title, const char* message);
    std::vector<uint8_t> BuildStatePayload() const;
    bool ParseTlv(uint8_t expected_type, const uint8_t* payload, size_t payload_len, size_t& offset, const uint8_t*& value, uint16_t& len) const;
    std::string ParseUtf8String(const uint8_t* data, size_t len) const;
};

#endif // _EINK_BLE_SERVICE_H_

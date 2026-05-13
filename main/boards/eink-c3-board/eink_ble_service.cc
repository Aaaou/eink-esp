#include "eink_ble_service.h"

#include "eink_epaper_display.h"
#include "eink_scene_legacy.h"

#include <cstring>
#include <string>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_check.h>
#include <esp_gatt_common_api.h>
#include <esp_log.h>
#include <esp_app_desc.h>

static const char* TAG = "EinkBleService";

static const uint8_t kServiceUuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};
static const uint8_t kRxUuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
static const uint8_t kTxUuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};
static const uint8_t kStateUuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E
};

static uint8_t kDummyValue = 0x00;

static uint8_t kCharPropWrite = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static uint8_t kCharPropNotify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t kCharPropRead = ESP_GATT_CHAR_PROP_BIT_READ;
static uint16_t kPrimaryServiceUuid = ESP_GATT_UUID_PRI_SERVICE;
static uint16_t kCharacterDeclUuid = ESP_GATT_UUID_CHAR_DECLARE;
static uint16_t kClientConfigUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static uint16_t kCccdDefault = 0x0000;

static esp_ble_gap_ext_adv_params_t kExtAdvParams = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND,
    .interval_min = 0x40,
    .interval_max = 0x60,
    .channel_map = ADV_CHNL_ALL,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
    .primary_phy = ESP_BLE_GAP_PHY_1M,
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,
    .sid = 0,
    .scan_req_notif = false,
};

static esp_ble_gap_ext_adv_t kExtAdvStartParams = {
    .instance = EinkBleService::kAdvInstance,
    .duration = 0,
    .max_events = 0,
};

static const esp_gatts_attr_db_t kGattDb[EinkBleService::kHandleCount] = {
    [EinkBleService::kSvc] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&kPrimaryServiceUuid, ESP_GATT_PERM_READ,
         sizeof(kServiceUuid128), sizeof(kServiceUuid128), (uint8_t*)kServiceUuid128}
    },
    [EinkBleService::kRxCharDecl] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&kCharacterDeclUuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &kCharPropWrite}
    },
    [EinkBleService::kRxCharValue] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t*)kRxUuid128, ESP_GATT_PERM_WRITE,
         512, 0, nullptr}
    },
    [EinkBleService::kTxCharDecl] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&kCharacterDeclUuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &kCharPropNotify}
    },
    [EinkBleService::kTxCharValue] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t*)kTxUuid128, ESP_GATT_PERM_READ,
         512, sizeof(kDummyValue), &kDummyValue}
    },
    [EinkBleService::kTxCharCccd] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&kClientConfigUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&kCccdDefault}
    },
    [EinkBleService::kStateCharDecl] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&kCharacterDeclUuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &kCharPropRead}
    },
    [EinkBleService::kStateCharValue] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t*)kStateUuid128, ESP_GATT_PERM_READ,
         128, sizeof(kDummyValue), &kDummyValue}
    },
};

EinkBleService* EinkBleService::s_instance = nullptr;

EinkBleService::EinkBleService(EinkEpaperDisplay* display) : display_(display) {
}

esp_err_t EinkBleService::Start() {
    if (started_) {
        return ESP_OK;
    }
    s_instance = this;
    ESP_RETURN_ON_ERROR(InitBleStack(), TAG, "init BLE stack failed");
    ESP_RETURN_ON_ERROR(ConfigureAdvertising(), TAG, "configure advertising failed");
    ESP_RETURN_ON_ERROR(RegisterService(), TAG, "register service failed");
    UpdateDisplayForBleMessage("EPD BLE", "WAIT MINI APP");
    started_ = true;
    return ESP_OK;
}

void EinkBleService::GapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (s_instance != nullptr) {
        s_instance->HandleGapEvent(event, param);
    }
}

void EinkBleService::GattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
    if (s_instance != nullptr) {
        s_instance->HandleGattsEvent(event, gatts_if, param);
    }
}

esp_err_t EinkBleService::InitBleStack() {
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(GapEventHandler), TAG, "register gap callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(GattsEventHandler), TAG, "register gatts callback failed");
    return ESP_OK;
}

esp_err_t EinkBleService::RegisterService() {
    return esp_ble_gatts_app_register(kAppId);
}

esp_err_t EinkBleService::ConfigureAdvertising() {
    static const uint8_t adv_raw[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0x00, 0x18,
        0x08, 0x09, 'E', 'I', 'N', 'K', '-', 'C', '3',
    };

    ESP_RETURN_ON_ERROR(
        esp_ble_gap_ext_adv_set_params(kAdvInstance, &kExtAdvParams),
        TAG,
        "set ext adv params failed");
    ESP_RETURN_ON_ERROR(
        esp_ble_gap_config_ext_adv_data_raw(kAdvInstance, sizeof(adv_raw), adv_raw),
        TAG,
        "set ext adv raw failed");
    return ESP_OK;
}

void EinkBleService::StartAdvertising() {
    esp_ble_gap_set_device_name("EINK-C3");
    esp_ble_gap_ext_adv_start(1, &kExtAdvStartParams);
}

void EinkBleService::HandleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
            esp_ble_gap_ext_adv_start(1, &kExtAdvStartParams);
            ESP_LOGI(TAG, "BLE advertising started");
            break;
        default:
            break;
    }
}

void EinkBleService::HandleRead(const esp_ble_gatts_cb_param_t::gatts_read_evt_param& read) {
    esp_gatt_rsp_t rsp = {};
    rsp.attr_value.handle = read.handle;
    auto payload = BuildStatePayload();
    rsp.attr_value.len = static_cast<uint16_t>(std::min<size_t>(payload.size(), sizeof(rsp.attr_value.value)));
    std::memcpy(rsp.attr_value.value, payload.data(), rsp.attr_value.len);
    esp_ble_gatts_send_response(gatts_if_, conn_id_, read.trans_id, ESP_GATT_OK, &rsp);
}

void EinkBleService::HandleWrite(const esp_ble_gatts_cb_param_t::gatts_write_evt_param& write) {
    esp_ble_gatts_send_response(gatts_if_, conn_id_, write.trans_id, ESP_GATT_OK, nullptr);

    if (write.handle == handles_[kTxCharCccd] && write.len == 2) {
        notify_enabled_ = (write.value[0] == 0x01);
        ESP_LOGI(TAG, "BLE notify %s", notify_enabled_ ? "enabled" : "disabled");
        return;
    }

    if (write.handle != handles_[kRxCharValue]) {
        return;
    }

    if (write.len < 10) {
        ESP_LOGW(TAG, "BLE packet too short: %u", write.len);
        NotifyTextResult(0x00, 0x01, "packet too short");
        return;
    }

    const uint8_t version = write.value[0];
    const uint8_t msg_type = write.value[1];
    const uint8_t opcode = write.value[2];
    const uint16_t session_id = static_cast<uint16_t>(write.value[4] | (write.value[5] << 8));
    const uint16_t seq = static_cast<uint16_t>(write.value[6] | (write.value[7] << 8));
    const uint16_t payload_len = static_cast<uint16_t>(write.value[8] | (write.value[9] << 8));
    const uint8_t* payload = write.value + 10;

    ESP_LOGI(TAG,
        "BLE RX packet: ver=%u msg=%u opcode=0x%02X session=%u seq=%u payload=%u total=%u",
        version,
        msg_type,
        opcode,
        session_id,
        seq,
        payload_len,
        write.len);

    switch (opcode) {
        case 0x01:
            NotifyTextResult(opcode, 0x00, "device info request received");
            break;
        case 0x10:
            {
                size_t tlv_offset = 0;
                const uint8_t* value = nullptr;
                uint16_t len = 0;
                time_t ts = time(nullptr);
                if (ParseTlv(0x01, payload, payload_len, tlv_offset, value, len) && len == 4) {
                    ts = static_cast<time_t>(value[0] | (value[1] << 8) | (value[2] << 16) | (value[3] << 24));
                }
                UpdateDisplayForBleMessage("BLE CAL", "CALENDAR CMD");
                eink_scene_show_calendar(ts);
                NotifyTextResult(opcode, 0x00, "calendar command received");
            }
            break;
        case 0x11:
            {
                size_t tlv_offset = 0;
                const uint8_t* value = nullptr;
                uint16_t len = 0;
                uint8_t item_count = 0;
                eink_memo_item_t memos[EINK_MEMO_MAX_ITEMS] = {};
                size_t parsed_count = 0;

                if (ParseTlv(0x01, payload, payload_len, tlv_offset, value, len) && len == 1) {
                    item_count = value[0];
                }

                for (uint8_t i = 0; i < item_count && i < EINK_MEMO_MAX_ITEMS; ++i) {
                    if (!ParseTlv(static_cast<uint8_t>(0x10 + i), payload, payload_len, tlv_offset, value, len) || len < 1) {
                        continue;
                    }
                    memos[parsed_count].checked = value[0] != 0;
                    std::string text = ParseUtf8String(value + 1, len - 1);
                    std::strncpy(memos[parsed_count].text, text.c_str(), EINK_MEMO_TEXT_MAX - 1);
                    memos[parsed_count].text[EINK_MEMO_TEXT_MAX - 1] = '\0';
                    ++parsed_count;
                }

                if (parsed_count == 0) {
                    NotifyTextResult(opcode, 0x01, "memo payload empty");
                    break;
                }

                UpdateDisplayForBleMessage("BLE MEMO", "MEMO CMD");
                eink_scene_show_memos(memos, parsed_count);
                NotifyTextResult(opcode, 0x00, "memo command received");
            }
            break;
        case 0x12:
            {
                frame_buffer_.clear();
                frame_buffer_.reserve(5513);
                UpdateDisplayForBleMessage("BLE IMG", "BEGIN UPLOAD");
                NotifyTextResult(opcode, 0x00, "frame upload begin");
            }
            break;
        case 0x13:
            {
                size_t tlv_offset = 0;
                const uint8_t* value = nullptr;
                uint16_t len = 0;
                uint32_t chunk_offset = 0;
                if (ParseTlv(0x01, payload, payload_len, tlv_offset, value, len) && len == 4) {
                    chunk_offset = static_cast<uint32_t>(value[0] | (value[1] << 8) | (value[2] << 16) | (value[3] << 24));
                }
                if (ParseTlv(0x02, payload, payload_len, tlv_offset, value, len)) {
                    if (frame_buffer_.size() < chunk_offset) {
                        frame_buffer_.resize(chunk_offset, 0x00);
                    }
                    if (frame_buffer_.size() == chunk_offset) {
                        frame_buffer_.insert(frame_buffer_.end(), value, value + len);
                    } else {
                        if ((chunk_offset + len) > frame_buffer_.size()) {
                            frame_buffer_.resize(chunk_offset + len, 0x00);
                        }
                        std::memcpy(frame_buffer_.data() + chunk_offset, value, len);
                    }
                }
                NotifyTextResult(opcode, 0x00, "frame chunk received");
            }
            break;
        case 0x14:
            {
                UpdateDisplayForBleMessage("BLE IMG", "UPLOAD DONE");
                if (frame_buffer_.size() >= 5513) {
                    eink_scene_show_frame(frame_buffer_.data() + 1, frame_buffer_.data() + 1 + 2756);
                    NotifyTextResult(opcode, 0x00, "frame upload end");
                } else {
                    NotifyTextResult(opcode, 0x11, "frame size mismatch");
                }
            }
            break;
        default:
            NotifyTextResult(opcode, 0x01, "opcode not handled yet");
            break;
    }
}

void EinkBleService::HandleGattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            gatts_if_ = gatts_if;
            esp_ble_gap_set_device_name("EINK-C3");
            esp_ble_gatts_create_attr_tab(kGattDb, gatts_if, kHandleCount, 0);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status == ESP_GATT_OK) {
                std::memcpy(handles_.data(), param->add_attr_tab.handles, sizeof(uint16_t) * kHandleCount);
                esp_ble_gatts_start_service(handles_[kSvc]);
                StartAdvertising();
            }
            break;
        case ESP_GATTS_CONNECT_EVT:
            connected_ = true;
            conn_id_ = param->connect.conn_id;
            ESP_LOGI(TAG, "BLE connected");
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            connected_ = false;
            notify_enabled_ = false;
            ESP_LOGI(TAG, "BLE disconnected");
            esp_ble_gap_ext_adv_start(1, &kExtAdvStartParams);
            break;
        case ESP_GATTS_READ_EVT:
            HandleRead(param->read);
            break;
        case ESP_GATTS_WRITE_EVT:
            HandleWrite(param->write);
            break;
        default:
            break;
    }
}

void EinkBleService::NotifyTextResult(uint8_t opcode, uint8_t result_code, const char* message) {
    if (!connected_ || !notify_enabled_ || gatts_if_ == ESP_GATT_IF_NONE) {
        return;
    }

    std::string text = message ? message : "";
    std::vector<uint8_t> packet;
    packet.reserve(10 + 3 + 1 + 3 + 1 + 3 + text.size());

    auto push_u8 = [&packet](uint8_t v) { packet.push_back(v); };
    auto push_u16 = [&packet](uint16_t v) {
        packet.push_back(static_cast<uint8_t>(v & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    payload.push_back(0x01); payload.push_back(0x00); payload.push_back(opcode);
    payload.push_back(0x02);
    payload.push_back(0x01); payload.push_back(0x00); payload.push_back(result_code);
    if (!text.empty()) {
        payload.push_back(0x03);
        payload.push_back(static_cast<uint8_t>(text.size() & 0xFF));
        payload.push_back(static_cast<uint8_t>((text.size() >> 8) & 0xFF));
        payload.insert(payload.end(), text.begin(), text.end());
    }

    push_u8(0x01);
    push_u8(0x05);
    push_u8(0x21);
    push_u8(0x00);
    push_u16(0x0000);
    push_u16(0x0000);
    push_u16(static_cast<uint16_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());

    esp_ble_gatts_send_indicate(
        gatts_if_,
        conn_id_,
        handles_[kTxCharValue],
        static_cast<uint16_t>(packet.size()),
        packet.data(),
        false);
}

void EinkBleService::UpdateDisplayForBleMessage(const char* title, const char* message) {
    if (display_ == nullptr) {
        return;
    }
    display_->SetModeMessage(title, message);
}

std::vector<uint8_t> EinkBleService::BuildStatePayload() const {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    const char* mode_text = "epaper-ble";
    const char* fw = app_desc ? app_desc->version : "unknown";
    const char* hw = "eink-c3-board";
    std::vector<uint8_t> payload;
    payload.reserve(64);
    payload.push_back(0x01);
    payload.push_back(static_cast<uint8_t>(std::strlen(fw) & 0xFF));
    payload.push_back(static_cast<uint8_t>((std::strlen(fw) >> 8) & 0xFF));
    payload.insert(payload.end(), fw, fw + std::strlen(fw));
    payload.push_back(0x02);
    payload.push_back(static_cast<uint8_t>(std::strlen(hw) & 0xFF));
    payload.push_back(static_cast<uint8_t>((std::strlen(hw) >> 8) & 0xFF));
    payload.insert(payload.end(), hw, hw + std::strlen(hw));
    payload.push_back(0x03);
    payload.push_back(0x02); payload.push_back(0x00);
    payload.push_back(104); payload.push_back(0);
    payload.push_back(0x04);
    payload.push_back(0x02); payload.push_back(0x00);
    payload.push_back(212); payload.push_back(0);
    payload.push_back(0x05);
    payload.push_back(0x01); payload.push_back(0x00);
    payload.push_back(1);
    payload.push_back(0x06);
    payload.push_back(0x02); payload.push_back(0x00);
    payload.push_back(180); payload.push_back(0);
    payload.push_back(0x07);
    payload.push_back(0x01); payload.push_back(0x00);
    payload.push_back(0);
    payload.push_back(0x08);
    payload.push_back(static_cast<uint8_t>(std::strlen(mode_text) & 0xFF));
    payload.push_back(static_cast<uint8_t>((std::strlen(mode_text) >> 8) & 0xFF));
    payload.insert(payload.end(), mode_text, mode_text + std::strlen(mode_text));
    return payload;
}

bool EinkBleService::ParseTlv(uint8_t expected_type, const uint8_t* payload, size_t payload_len, size_t& offset, const uint8_t*& value, uint16_t& len) const {
    if (offset + 3 > payload_len) {
        return false;
    }
    uint8_t type = payload[offset];
    len = static_cast<uint16_t>(payload[offset + 1] | (payload[offset + 2] << 8));
    if ((offset + 3 + len) > payload_len) {
        return false;
    }
    value = payload + offset + 3;
    offset += 3 + len;
    return type == expected_type;
}

std::string EinkBleService::ParseUtf8String(const uint8_t* data, size_t len) const {
    if (data == nullptr || len == 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + len);
}

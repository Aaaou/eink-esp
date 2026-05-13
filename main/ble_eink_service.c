#include "ble_eink_service.h"

#include "eink_panel.h"
#include "eink_scene.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ble_eink";

enum {
    HANDLE_SVC = 0,
    HANDLE_RX_DECL,
    HANDLE_RX_VALUE,
    HANDLE_TX_DECL,
    HANDLE_TX_VALUE,
    HANDLE_TX_CCCD,
    HANDLE_STATE_DECL,
    HANDLE_STATE_VALUE,
    HANDLE_COUNT,
};

static const uint16_t APP_ID = 0x45E1;
static const uint8_t ADV_INSTANCE = 0;
static const size_t FRAME_TOTAL_SIZE = 1 + (EINK_PANEL_BUF_LEN * 2);

static const uint8_t SERVICE_UUID[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
};
static const uint8_t RX_UUID[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E,
};
static const uint8_t TX_UUID[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E,
};
static const uint8_t STATE_UUID[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E,
};

static uint8_t s_dummy_value = 0;
static uint8_t s_rx_prop = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static uint8_t s_tx_prop = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t s_state_prop = ESP_GATT_CHAR_PROP_BIT_READ;
static uint16_t s_primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static uint16_t s_character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static uint16_t s_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static uint16_t s_cccd_default = 0;

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id;
static uint16_t s_handles[HANDLE_COUNT];
static bool s_connected;
static bool s_notify_enabled;
static bool s_service_ready;
static bool s_advertising;
static uint8_t *s_frame_buffer;
static size_t s_frame_len;
static uint8_t *s_frame_received;
static size_t s_frame_received_len;
static uint8_t *s_prepare_buffer;
static size_t s_prepare_len;
static uint8_t *s_rx_packet_buffer;
static size_t s_rx_packet_len;
static size_t s_rx_packet_expected;
static uint8_t s_rx_packet_opcode;
static uint8_t s_rx_packet_msg_type;
static uint16_t s_rx_packet_session;
static QueueHandle_t s_display_queue;

typedef enum {
    DISPLAY_JOB_CALENDAR,
    DISPLAY_JOB_MEMO,
    DISPLAY_JOB_FRAME,
} display_job_type_t;

typedef struct {
    display_job_type_t type;
    uint8_t opcode;
    union {
        time_t calendar_ts;
        struct {
            eink_memo_item_t items[EINK_MEMO_MAX_ITEMS];
            size_t count;
        } memo;
        struct {
            uint8_t *frame;
            size_t len;
        } frame;
    } data;
} display_job_t;

static esp_ble_gap_ext_adv_params_t s_adv_params = {
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

static esp_ble_gap_ext_adv_t s_adv_start_params = {
    .instance = ADV_INSTANCE,
    .duration = 0,
    .max_events = 0,
};

static const esp_gatts_attr_db_t GATT_DB[HANDLE_COUNT] = {
    [HANDLE_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&s_primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(SERVICE_UUID), sizeof(SERVICE_UUID), (uint8_t *)SERVICE_UUID},
    },
    [HANDLE_RX_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&s_character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &s_rx_prop},
    },
    [HANDLE_RX_VALUE] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t *)RX_UUID, ESP_GATT_PERM_WRITE,
         512, 0, NULL},
    },
    [HANDLE_TX_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&s_character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &s_tx_prop},
    },
    [HANDLE_TX_VALUE] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t *)TX_UUID, ESP_GATT_PERM_READ,
         512, sizeof(s_dummy_value), &s_dummy_value},
    },
    [HANDLE_TX_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&s_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t *)&s_cccd_default},
    },
    [HANDLE_STATE_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&s_character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), &s_state_prop},
    },
    [HANDLE_STATE_VALUE] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t *)STATE_UUID, ESP_GATT_PERM_READ,
         128, sizeof(s_dummy_value), &s_dummy_value},
    },
};

static bool parse_tlv(uint8_t expected_type, const uint8_t *payload, size_t payload_len, size_t *offset, const uint8_t **value, uint16_t *len)
{
    if ((*offset + 3) > payload_len) {
        return false;
    }

    uint8_t type = payload[*offset];
    *len = (uint16_t)(payload[*offset + 1] | (payload[*offset + 2] << 8));
    if ((*offset + 3 + *len) > payload_len) {
        return false;
    }

    *value = payload + *offset + 3;
    *offset += 3 + *len;
    return type == expected_type;
}

static bool parse_tlv_header(const uint8_t *payload, size_t payload_len, size_t *offset, uint8_t *type, const uint8_t **value, uint16_t *len)
{
    if ((*offset + 3) > payload_len) {
        return false;
    }

    *type = payload[*offset];
    *len = (uint16_t)(payload[*offset + 1] | (payload[*offset + 2] << 8));
    *value = payload + *offset + 3;
    *offset += 3;
    return true;
}

static bool parse_tlv_partial(uint8_t expected_type, const uint8_t *payload, size_t payload_len, size_t *offset, const uint8_t **value, uint16_t *len)
{
    uint8_t type = 0;
    if (!parse_tlv_header(payload, payload_len, offset, &type, value, len) || type != expected_type) {
        return false;
    }
    if ((*offset + *len) > payload_len) {
        *len = (uint16_t)(payload_len - *offset);
    }
    *offset += *len;
    return true;
}

static void notify_text_result(uint8_t opcode, uint8_t result_code, const char *message)
{
    (void)message;
    if (!s_connected || !s_notify_enabled || s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }

    uint8_t packet[20];
    size_t payload_len;

    packet[0] = 0x01;
    packet[1] = 0x05;
    packet[2] = 0x21;
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = 0x00;
    packet[6] = 0x00;
    packet[7] = 0x00;

    size_t off = 10;
    packet[off++] = 0x01;
    packet[off++] = 0x01;
    packet[off++] = 0x00;
    packet[off++] = opcode;
    packet[off++] = 0x02;
    packet[off++] = 0x01;
    packet[off++] = 0x00;
    packet[off++] = result_code;
    payload_len = off - 10;
    packet[8] = (uint8_t)(payload_len & 0xFF);
    packet[9] = (uint8_t)((payload_len >> 8) & 0xFF);

    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[HANDLE_TX_VALUE], (uint16_t)off, packet, false);
}

static void free_display_job_payload(display_job_t *job)
{
    if ((job != NULL) && (job->type == DISPLAY_JOB_FRAME)) {
        free(job->data.frame.frame);
        job->data.frame.frame = NULL;
        job->data.frame.len = 0;
    }
}

static bool queue_display_job(display_job_t *job)
{
    if (s_display_queue == NULL) {
        return false;
    }
    if (xQueueSend(s_display_queue, job, 0) == pdTRUE) {
        return true;
    }

    display_job_t dropped_job = {0};
    if (xQueueReceive(s_display_queue, &dropped_job, 0) == pdTRUE) {
        free_display_job_payload(&dropped_job);
    }
    return xQueueSend(s_display_queue, job, 0) == pdTRUE;
}

static void display_task(void *arg)
{
    (void)arg;
    display_job_t job;

    while (true) {
        if (xQueueReceive(s_display_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = ESP_FAIL;
        switch (job.type) {
        case DISPLAY_JOB_CALENDAR:
            err = eink_scene_show_calendar(job.data.calendar_ts);
            break;
        case DISPLAY_JOB_MEMO:
            err = eink_scene_show_memos(job.data.memo.items, job.data.memo.count);
            break;
        case DISPLAY_JOB_FRAME:
            if (job.data.frame.frame != NULL && job.data.frame.len >= FRAME_TOTAL_SIZE) {
                err = eink_scene_show_frame(
                    job.data.frame.frame + 1,
                    job.data.frame.frame + 1 + EINK_PANEL_BUF_LEN);
            }
            free(job.data.frame.frame);
            break;
        default:
            break;
        }
        notify_text_result(job.opcode, err == ESP_OK ? 0x00 : 0x10, err == ESP_OK ? "display ok" : "display failed");
    }
}

static void send_state_response(const esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_rsp_t rsp = {0};
    uint8_t payload[20];
    size_t off = 0;

    rsp.attr_value.handle = param->read.handle;

#define PUSH_TLV_U16(type, value) do { \
        payload[off++] = (type); payload[off++] = 2; payload[off++] = 0; \
        payload[off++] = (uint8_t)((value) & 0xFF); \
        payload[off++] = (uint8_t)(((value) >> 8) & 0xFF); \
    } while (0)
#define PUSH_TLV_U8(type, value) do { \
        payload[off++] = (type); payload[off++] = 1; payload[off++] = 0; payload[off++] = (value); \
    } while (0)

    PUSH_TLV_U16(0x03, EINK_PANEL_WIDTH);
    PUSH_TLV_U16(0x04, EINK_PANEL_HEIGHT);
    PUSH_TLV_U8(0x05, 2);
    PUSH_TLV_U8(0x07, 0);

#undef PUSH_TLV_U16
#undef PUSH_TLV_U8

    rsp.attr_value.len = (uint16_t)off;
    memcpy(rsp.attr_value.value, payload, off);
    esp_ble_gatts_send_response(s_gatts_if, s_conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
}

static void handle_command(uint8_t opcode, const uint8_t *payload, size_t payload_len)
{
    switch (opcode) {
    case 0x01:
        notify_text_result(opcode, 0x00, "device info ok");
        break;
    case 0x10: {
        size_t off = 0;
        const uint8_t *value = NULL;
        uint16_t len = 0;
        display_job_t job = {
            .type = DISPLAY_JOB_CALENDAR,
            .opcode = opcode,
        };
        job.data.calendar_ts = time(NULL);
        if (parse_tlv(0x01, payload, payload_len, &off, &value, &len) && len == 4) {
            job.data.calendar_ts = (time_t)((uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24));
        }
        notify_text_result(opcode, queue_display_job(&job) ? 0x00 : 0x30, "calendar queued");
        break;
    }
    case 0x11: {
        size_t off = 0;
        const uint8_t *value = NULL;
        uint16_t len = 0;
        uint8_t item_count = 0;
        eink_memo_item_t memos[EINK_MEMO_MAX_ITEMS] = {0};
        size_t parsed = 0;

        ESP_LOGI(TAG, "memo payload=%u", (unsigned int)payload_len);
        if (parse_tlv_partial(0x01, payload, payload_len, &off, &value, &len) && len >= 1) {
            item_count = value[0];
        }
        for (uint8_t i = 0; i < item_count && i < EINK_MEMO_MAX_ITEMS; ++i) {
            if (!parse_tlv_partial((uint8_t)(0x10 + i), payload, payload_len, &off, &value, &len) || len < 1) {
                continue;
            }
            memos[parsed].checked = value[0] != 0;
            size_t text_len = len - 1;
            if (text_len >= EINK_MEMO_TEXT_MAX) {
                text_len = EINK_MEMO_TEXT_MAX - 1;
            }
            memcpy(memos[parsed].text, value + 1, text_len);
            memos[parsed].text[text_len] = '\0';
            ESP_LOGI(TAG, "memo item %u checked=%u text_len=%u", (unsigned int)parsed, memos[parsed].checked ? 1 : 0, (unsigned int)text_len);
            parsed++;
        }
        if (parsed == 0) {
            ESP_LOGW(TAG, "memo empty after parse item_count=%u payload=%u", item_count, (unsigned int)payload_len);
            notify_text_result(opcode, 0x01, "memo empty");
            break;
        }
        display_job_t job = {
            .type = DISPLAY_JOB_MEMO,
            .opcode = opcode,
        };
        memcpy(job.data.memo.items, memos, sizeof(memos));
        job.data.memo.count = parsed;
        notify_text_result(opcode, queue_display_job(&job) ? 0x00 : 0x30, "memo queued");
        break;
    }
    case 0x12:
        free(s_frame_buffer);
        free(s_frame_received);
        s_frame_buffer = malloc(FRAME_TOTAL_SIZE);
        s_frame_received = calloc(FRAME_TOTAL_SIZE, 1);
        s_frame_received_len = FRAME_TOTAL_SIZE;
        s_frame_len = 0;
        ESP_LOGI(TAG, "frame begin expected=%u begin_payload=%u", (unsigned int)FRAME_TOTAL_SIZE, (unsigned int)payload_len);
        notify_text_result(opcode, (s_frame_buffer && s_frame_received) ? 0x00 : 0x20, (s_frame_buffer && s_frame_received) ? "frame begin" : "no memory");
        break;
    case 0x13: {
        size_t off = 0;
        const uint8_t *value = NULL;
        uint16_t len = 0;
        uint32_t chunk_offset = 0;
        uint8_t type = 0;
        if (!s_frame_buffer) {
            notify_text_result(opcode, 0x21, "frame not started");
            break;
        }
        if (parse_tlv(0x01, payload, payload_len, &off, &value, &len) && len == 4) {
            chunk_offset = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
        }
        if (parse_tlv_header(payload, payload_len, &off, &type, &value, &len) && type == 0x02) {
            size_t available_len = payload_len - off;
            if (len > available_len) {
                ESP_LOGW(TAG, "frame chunk data truncated declared=%u available=%u", len, (unsigned int)available_len);
                len = (uint16_t)available_len;
            }
        } else {
            len = 0;
        }
        if (len > 0 && (chunk_offset + len) <= FRAME_TOTAL_SIZE) {
            memcpy(s_frame_buffer + chunk_offset, value, len);
            if (s_frame_received != NULL) {
                memset(s_frame_received + chunk_offset, 1, len);
            }
            if ((chunk_offset + len) > s_frame_len) {
                s_frame_len = chunk_offset + len;
            }
            ESP_LOGI(TAG, "frame chunk offset=%u len=%u total=%u/%u",
                (unsigned int)chunk_offset,
                (unsigned int)len,
                (unsigned int)s_frame_len,
                (unsigned int)FRAME_TOTAL_SIZE);
            notify_text_result(opcode, 0x00, "chunk ok");
        } else {
            ESP_LOGW(TAG, "bad frame chunk offset=%u payload=%u", (unsigned int)chunk_offset, (unsigned int)payload_len);
            notify_text_result(opcode, 0x22, "bad chunk");
        }
        break;
    }
    case 0x14: {
        size_t off = 0;
        const uint8_t *value = NULL;
        uint16_t len = 0;
        uint32_t declared_total = 0;
        size_t received_count = 0;
        size_t first_missing = FRAME_TOTAL_SIZE;
        if (s_frame_received != NULL) {
            for (size_t i = 0; i < s_frame_received_len; ++i) {
                if (s_frame_received[i]) {
                    received_count++;
                } else if (first_missing == FRAME_TOTAL_SIZE) {
                    first_missing = i;
                }
            }
        }
        if (parse_tlv(0x02, payload, payload_len, &off, &value, &len) && len == 4) {
            declared_total = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
        }
        ESP_LOGI(TAG, "frame end received=%u/%u high_water=%u expected=%u first_missing=%u declared_total=%u end_payload=%u",
            (unsigned int)received_count,
            (unsigned int)FRAME_TOTAL_SIZE,
            (unsigned int)s_frame_len,
            (unsigned int)FRAME_TOTAL_SIZE,
            (unsigned int)first_missing,
            (unsigned int)declared_total,
            (unsigned int)payload_len);
        if (s_frame_buffer && received_count >= FRAME_TOTAL_SIZE) {
            display_job_t job = {
                .type = DISPLAY_JOB_FRAME,
                .opcode = opcode,
            };
            job.data.frame.frame = s_frame_buffer;
            job.data.frame.len = s_frame_len;
            if (queue_display_job(&job)) {
                s_frame_buffer = NULL;
                s_frame_len = 0;
                notify_text_result(opcode, 0x00, "frame queued");
            } else {
                notify_text_result(opcode, 0x30, "display busy");
            }
        } else {
            notify_text_result(opcode, 0x11, "frame size mismatch");
        }
        if (s_frame_buffer != NULL) {
            free(s_frame_buffer);
            s_frame_buffer = NULL;
            s_frame_len = 0;
        }
        free(s_frame_received);
        s_frame_received = NULL;
        s_frame_received_len = 0;
        break;
    }
    default:
        notify_text_result(opcode, 0x01, "opcode not handled");
        break;
    }
}

static void process_complete_rx_packet(const uint8_t *data, size_t len)
{
    if (len < 10) {
        ESP_LOGW(TAG, "rx short packet len=%u", (unsigned int)len);
        notify_text_result(0x00, 0x01, "packet too short");
        return;
    }

    uint8_t version = data[0];
    uint8_t msg_type = data[1];
    uint8_t opcode = data[2];
    uint8_t flags = data[3];
    uint16_t session = (uint16_t)(data[4] | (data[5] << 8));
    uint16_t seq = (uint16_t)(data[6] | (data[7] << 8));
    uint16_t payload_len = (uint16_t)(data[8] | (data[9] << 8));
    if (version != 1 || (size_t)payload_len > (len - 10)) {
        ESP_LOGW(TAG, "bad packet version=%u msg=0x%02x opcode=0x%02x flags=0x%02x session=%u seq=%u len=%u payload=%u",
            version,
            msg_type,
            opcode,
            flags,
            session,
            seq,
            (unsigned int)len,
            payload_len);
        notify_text_result(opcode, 0x02, "bad packet");
        return;
    }
    ESP_LOGI(TAG, "rx msg=0x%02x opcode=0x%02x flags=0x%02x session=%u seq=%u payload=%u",
        msg_type,
        opcode,
        flags,
        session,
        seq,
        payload_len);
    handle_command(opcode, data + 10, payload_len);
}

static void clear_rx_packet_buffer(void)
{
    free(s_rx_packet_buffer);
    s_rx_packet_buffer = NULL;
    s_rx_packet_len = 0;
    s_rx_packet_expected = 0;
    s_rx_packet_opcode = 0;
    s_rx_packet_msg_type = 0;
    s_rx_packet_session = 0;
}

static bool append_rx_packet_bytes(const uint8_t *data, size_t len, uint8_t flags, uint16_t seq)
{
    if (s_rx_packet_buffer == NULL) {
        return false;
    }

    size_t copy_len = len;
    if ((s_rx_packet_len + copy_len) > s_rx_packet_expected) {
        copy_len = s_rx_packet_expected - s_rx_packet_len;
    }
    memcpy(s_rx_packet_buffer + s_rx_packet_len, data, copy_len);
    s_rx_packet_len += copy_len;
    ESP_LOGI(TAG, "rx fragment append opcode=0x%02x flags=0x%02x seq=%u got=%u/%u",
        s_rx_packet_opcode,
        flags,
        seq,
        (unsigned int)s_rx_packet_len,
        (unsigned int)s_rx_packet_expected);

    if (s_rx_packet_len >= s_rx_packet_expected) {
        process_complete_rx_packet(s_rx_packet_buffer, s_rx_packet_len);
        clear_rx_packet_buffer();
    }
    return true;
}

static void process_rx_packet(const uint8_t *data, size_t len)
{
    if (s_rx_packet_buffer != NULL && s_rx_packet_len < s_rx_packet_expected) {
        append_rx_packet_bytes(data, len, 0, 0);
        return;
    }

    if (len < 10) {
        if (append_rx_packet_bytes(data, len, 0, 0)) {
            return;
        }
        ESP_LOGW(TAG, "rx short packet len=%u", (unsigned int)len);
        notify_text_result(0x00, 0x01, "packet too short");
        return;
    }

    uint8_t version = data[0];
    uint8_t msg_type = data[1];
    uint8_t opcode = data[2];
    uint8_t flags = data[3];
    uint16_t session = (uint16_t)(data[4] | (data[5] << 8));
    uint16_t seq = (uint16_t)(data[6] | (data[7] << 8));
    uint16_t payload_len = (uint16_t)(data[8] | (data[9] << 8));
    size_t packet_expected = (size_t)payload_len + 10;
    size_t payload_part_len = len - 10;

    if (version != 1) {
        if (append_rx_packet_bytes(data, len, 0, 0)) {
            return;
        }
        ESP_LOGW(TAG, "bad packet version=%u len=%u", version, (unsigned int)len);
        notify_text_result(opcode, 0x02, "bad packet");
        return;
    }

    if (payload_len <= (len - 10)) {
        process_complete_rx_packet(data, len);
        return;
    }

    if (opcode == 0x12 || opcode == 0x14) {
        ESP_LOGI(TAG, "rx short command accepted msg=0x%02x opcode=0x%02x flags=0x%02x session=%u seq=%u got=%u/%u",
            msg_type,
            opcode,
            flags,
            session,
            seq,
            (unsigned int)len,
            (unsigned int)packet_expected);
        clear_rx_packet_buffer();
        handle_command(opcode, data + 10, payload_part_len);
        return;
    }

    if ((s_rx_packet_buffer == NULL) ||
        (s_rx_packet_opcode != opcode) ||
        (s_rx_packet_msg_type != msg_type) ||
        (s_rx_packet_session != session) ||
        (s_rx_packet_len >= s_rx_packet_expected)) {
        clear_rx_packet_buffer();
        s_rx_packet_buffer = malloc(packet_expected);
        if (s_rx_packet_buffer == NULL) {
            notify_text_result(opcode, 0x20, "rx no memory");
            return;
        }
        memcpy(s_rx_packet_buffer, data, 10);
        memcpy(s_rx_packet_buffer + 10, data + 10, payload_part_len);
        s_rx_packet_len = 10 + payload_part_len;
        s_rx_packet_expected = packet_expected;
        s_rx_packet_opcode = opcode;
        s_rx_packet_msg_type = msg_type;
        s_rx_packet_session = session;
        ESP_LOGI(TAG, "rx fragment start msg=0x%02x opcode=0x%02x flags=0x%02x session=%u seq=%u got=%u/%u",
            msg_type,
            opcode,
            flags,
            session,
            seq,
            (unsigned int)s_rx_packet_len,
            (unsigned int)s_rx_packet_expected);
    } else {
        ESP_LOGI(TAG, "rx fragment header append msg=0x%02x opcode=0x%02x flags=0x%02x session=%u seq=%u",
            msg_type,
            opcode,
            flags,
            session,
            seq);
        append_rx_packet_bytes(data + 10, payload_part_len, flags, seq);
        return;
    }

    if (s_rx_packet_len >= s_rx_packet_expected) {
        process_complete_rx_packet(s_rx_packet_buffer, s_rx_packet_len);
        clear_rx_packet_buffer();
    }
}

static void clear_prepare_buffer(void)
{
    free(s_prepare_buffer);
    s_prepare_buffer = NULL;
    s_prepare_len = 0;
}

static esp_gatt_status_t append_prepare_write(const esp_ble_gatts_cb_param_t *param)
{
    const size_t next_len = (size_t)param->write.offset + (size_t)param->write.len;
    uint8_t *next_buffer;

    if (param->write.handle != s_handles[HANDLE_RX_VALUE]) {
        return ESP_GATT_OK;
    }
    if (next_len > 4096) {
        ESP_LOGW(TAG, "prepare write too large: %u", (unsigned int)next_len);
        return ESP_GATT_INVALID_ATTR_LEN;
    }

    next_buffer = realloc(s_prepare_buffer, next_len);
    if (next_buffer == NULL) {
        clear_prepare_buffer();
        return ESP_GATT_NO_RESOURCES;
    }

    s_prepare_buffer = next_buffer;
    memcpy(s_prepare_buffer + param->write.offset, param->write.value, param->write.len);
    if (next_len > s_prepare_len) {
        s_prepare_len = next_len;
    }
    ESP_LOGI(TAG, "prepare write offset=%u len=%u total=%u",
        (unsigned int)param->write.offset,
        (unsigned int)param->write.len,
        (unsigned int)s_prepare_len);
    return ESP_GATT_OK;
}

static void send_write_response(const esp_ble_gatts_cb_param_t *param, esp_gatt_status_t status)
{
    if (!param->write.need_rsp) {
        return;
    }

    esp_gatt_rsp_t rsp = {0};
    rsp.attr_value.handle = param->write.handle;
    rsp.attr_value.offset = param->write.offset;
    rsp.attr_value.len = param->write.len;
    if (rsp.attr_value.len > sizeof(rsp.attr_value.value)) {
        rsp.attr_value.len = sizeof(rsp.attr_value.value);
    }
    if (rsp.attr_value.len > 0) {
        memcpy(rsp.attr_value.value, param->write.value, rsp.attr_value.len);
    }
    esp_ble_gatts_send_response(s_gatts_if, s_conn_id, param->write.trans_id, status, param->write.is_prep ? &rsp : NULL);
}

static void handle_write(const esp_ble_gatts_cb_param_t *param)
{
    const esp_ble_gatts_cb_param_t *p = param;

    if (p->write.handle == s_handles[HANDLE_TX_CCCD] && p->write.len == 2) {
        send_write_response(p, ESP_GATT_OK);
        s_notify_enabled = p->write.value[0] == 0x01;
        ESP_LOGI(TAG, "notify %s", s_notify_enabled ? "enabled" : "disabled");
        return;
    }
    if (p->write.handle != s_handles[HANDLE_RX_VALUE]) {
        send_write_response(p, ESP_GATT_OK);
        return;
    }

    ESP_LOGI(TAG, "write rx len=%u offset=%u prep=%u need_rsp=%u",
        (unsigned int)p->write.len,
        (unsigned int)p->write.offset,
        p->write.is_prep ? 1 : 0,
        p->write.need_rsp ? 1 : 0);

    if (p->write.is_prep) {
        esp_gatt_status_t status = append_prepare_write(p);
        send_write_response(p, status);
        return;
    }

    send_write_response(p, ESP_GATT_OK);
    process_rx_packet(p->write.value, p->write.len);
}

static void start_advertising(void)
{
    if (!s_service_ready || s_advertising) {
        return;
    }
    esp_err_t err = esp_ble_gap_ext_adv_start(1, &s_adv_start_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start advertising failed: %s", esp_err_to_name(err));
    } else {
        s_advertising = true;
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
        start_advertising();
        break;
    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
        s_advertising = param->ext_adv_start.status == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(TAG, "advertising start status=%d", param->ext_adv_start.status);
        break;
    case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;
    default:
        break;
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name("EINK-C3");
        esp_ble_gatts_create_attr_tab(GATT_DB, gatts_if, HANDLE_COUNT, 0);
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK) {
            memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
            esp_ble_gatts_start_service(s_handles[HANDLE_SVC]);
            s_service_ready = true;
            start_advertising();
        }
        break;
    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        s_advertising = false;
        ESP_LOGI(TAG, "connected");
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_notify_enabled = false;
        s_advertising = false;
        ESP_LOGI(TAG, "disconnected");
        start_advertising();
        break;
    case ESP_GATTS_READ_EVT:
        send_state_response(param);
        break;
    case ESP_GATTS_WRITE_EVT:
        handle_write(param);
        break;
    case ESP_GATTS_EXEC_WRITE_EVT:
        if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
            ESP_LOGI(TAG, "exec write len=%u", (unsigned int)s_prepare_len);
            process_rx_packet(s_prepare_buffer, s_prepare_len);
        } else {
            ESP_LOGI(TAG, "exec write cancelled");
        }
        clear_prepare_buffer();
        esp_ble_gatts_send_response(gatts_if, param->exec_write.conn_id, param->exec_write.trans_id, ESP_GATT_OK, NULL);
        break;
    default:
        break;
    }
}

esp_err_t ble_eink_service_init(void)
{
    static uint8_t adv_raw[] = {
        0x02, 0x01, 0x06,
        0x11, 0x07,
        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
        0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
        0x08, 0x09, 'E', 'I', 'N', 'K', '-', 'C', '3',
    };

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_display_queue == NULL) {
        s_display_queue = xQueueCreate(1, sizeof(display_job_t));
        ESP_RETURN_ON_FALSE(s_display_queue != NULL, ESP_ERR_NO_MEM, TAG, "display queue failed");
        BaseType_t task_ok = xTaskCreate(display_task, "ble_eink_display", 4096, NULL, 4, NULL);
        ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "display task failed");
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_cb), TAG, "gap callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(gatts_cb), TAG, "gatts callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_ext_adv_set_params(ADV_INSTANCE, &s_adv_params), TAG, "adv params failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_ext_adv_data_raw(ADV_INSTANCE, sizeof(adv_raw), adv_raw), TAG, "adv data failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_app_register(APP_ID), TAG, "gatts register failed");
    ESP_LOGI(TAG, "BLE eink service initialized");
    return ESP_OK;
}

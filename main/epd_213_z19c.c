#include "epd_213_z19c.h"

#include "board_config.h"
#include "board_spi.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8574.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "epd_213_z19c";

#define Z19C_POWER_ON_TIME_MS        100U
#define Z19C_POWER_OFF_TIME_MS        50U
#define Z19C_FULL_REFRESH_TIME_MS  17000U
#define Z19C_PART_REFRESH_TIME_MS  17000U

static spi_device_handle_t s_spi;
static pcf8574_t s_iox;
static bool s_ready;
static bool s_power_is_on;
static bool s_initial_write = true;

static esp_err_t epd_write_command(uint8_t command)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &command,
    };

    gpio_set_level(BOARD_EINK_DC_GPIO, 0);
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t epd_write_data(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };

    gpio_set_level(BOARD_EINK_DC_GPIO, 1);
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t epd_write_data_byte(uint8_t data)
{
    return epd_write_data(&data, 1);
}

static esp_err_t epd_write_repeat(uint8_t value, size_t len)
{
    uint8_t chunk[32];
    memset(chunk, value, sizeof(chunk));

    while (len > 0) {
        const size_t now = (len > sizeof(chunk)) ? sizeof(chunk) : len;
        esp_err_t err = epd_write_data(chunk, now);
        if (err != ESP_OK) {
            return err;
        }
        len -= now;
    }

    return ESP_OK;
}

static void epd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t epd_wait_while_busy(uint32_t ms, const char *stage)
{
    /*
     * Align with official GxEPD2 behavior for EPD_BUSY = -1:
     * do not sample a busy pin, simply wait the panel-specific fixed time.
     */
    ESP_LOGI(TAG, "[√] %s，固定等待 %lums（BUSY未接，按官方方式处理）", stage, (unsigned long)ms);
    epd_delay_ms(ms);
    return ESP_OK;
}

static esp_err_t epd_power_4150b(bool on)
{
    ESP_RETURN_ON_ERROR(
        pcf8574_set_bit(&s_iox, BOARD_IOX_BIT_EINK_CTRL_4150B, on),
        TAG,
        "4150B control failed");

    if (on) {
        epd_delay_ms(BOARD_EINK_POWER_SETTLE_MS);
    }
    return ESP_OK;
}

static esp_err_t epd_reset_external(void)
{
    ESP_RETURN_ON_ERROR(
        pcf8574_set_bit(&s_iox, BOARD_IOX_BIT_EINK_RES, 0),
        TAG,
        "RES low failed");
    epd_delay_ms(BOARD_EINK_RESET_HOLD_MS);

    ESP_RETURN_ON_ERROR(
        pcf8574_set_bit(&s_iox, BOARD_IOX_BIT_EINK_RES, 1),
        TAG,
        "RES high failed");
    epd_delay_ms(200);
    return ESP_OK;
}

static esp_err_t epd_prepare_panel_once(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_DC_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "DC gpio init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_EINK_DC_GPIO, 0), TAG, "DC init level failed");
    ESP_RETURN_ON_ERROR(pcf8574_init(&s_iox, BOARD_IOX_ADDR, 0xFF), TAG, "pcf8574 init failed");
    ESP_RETURN_ON_ERROR(board_eink_spi_init(&s_spi), TAG, "spi init failed");

    s_ready = true;
    return ESP_OK;
}

static esp_err_t epd_begin_session(void)
{
    ESP_RETURN_ON_ERROR(epd_prepare_panel_once(), TAG, "panel prepare failed");
    ESP_RETURN_ON_ERROR(epd_power_4150b(true), TAG, "4150B on failed");
    ESP_RETURN_ON_ERROR(epd_reset_external(), TAG, "external reset failed");
    s_power_is_on = false;
    return ESP_OK;
}

static esp_err_t epd_set_partial_ram_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    const uint16_t xe = (uint16_t)((x + w - 1U) | 0x0007U);
    const uint16_t ye = (uint16_t)(y + h - 1U);
    x &= 0xFFF8U;

    ESP_RETURN_ON_ERROR(epd_write_command(0x90), TAG, "cmd 0x90 failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(x & 0xFF)), TAG, "data x failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(xe & 0xFF)), TAG, "data xe failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(y >> 8)), TAG, "data y hi failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(y & 0xFF)), TAG, "data y lo failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(ye >> 8)), TAG, "data ye hi failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte((uint8_t)(ye & 0xFF)), TAG, "data ye lo failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte(0x01), TAG, "data tail failed");
    return ESP_OK;
}

static esp_err_t epd_power_on(void)
{
    if (!s_power_is_on) {
        ESP_RETURN_ON_ERROR(epd_write_command(0x04), TAG, "cmd 0x04 failed");
        ESP_RETURN_ON_ERROR(
            epd_wait_while_busy(Z19C_POWER_ON_TIME_MS, "_PowerOn"),
            TAG,
            "power on wait failed");
        s_power_is_on = true;
    }
    return ESP_OK;
}

static esp_err_t epd_power_off(void)
{
    if (s_power_is_on) {
        ESP_RETURN_ON_ERROR(epd_write_command(0x02), TAG, "cmd 0x02 failed");
        ESP_RETURN_ON_ERROR(
            epd_wait_while_busy(Z19C_POWER_OFF_TIME_MS, "_PowerOff"),
            TAG,
            "power off wait failed");
        s_power_is_on = false;
    }
    return ESP_OK;
}

static esp_err_t epd_init_display(void)
{
    const uint8_t resolution[] = {
        EPD_213_Z19C_WIDTH,
        (uint8_t)(EPD_213_Z19C_HEIGHT >> 8),
        (uint8_t)(EPD_213_Z19C_HEIGHT & 0xFF),
    };

    ESP_RETURN_ON_ERROR(epd_write_command(0x00), TAG, "cmd 0x00 failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte(0x8F), TAG, "data 0x8F failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x61), TAG, "cmd 0x61 failed");
    ESP_RETURN_ON_ERROR(epd_write_data(resolution, sizeof(resolution)), TAG, "data 0x61 failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x50), TAG, "cmd 0x50 failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte(0x77), TAG, "data 0x77 failed");
    return ESP_OK;
}

static esp_err_t epd_init_part(void)
{
    ESP_RETURN_ON_ERROR(epd_init_display(), TAG, "init display failed");
    return epd_power_on();
}

static esp_err_t epd_update_part(void)
{
    ESP_RETURN_ON_ERROR(epd_write_command(0x12), TAG, "cmd 0x12 failed");
    return epd_wait_while_busy(Z19C_PART_REFRESH_TIME_MS, "_Update_Part");
}

static esp_err_t epd_update_full(void)
{
    ESP_RETURN_ON_ERROR(epd_write_command(0x12), TAG, "cmd 0x12 failed");
    return epd_wait_while_busy(Z19C_FULL_REFRESH_TIME_MS, "_Update_Full");
}

static esp_err_t epd_write_screen_buffer(uint8_t black_value, uint8_t color_value)
{
    s_initial_write = false;

    ESP_RETURN_ON_ERROR(epd_init_part(), TAG, "write screen buffer init failed");
    ESP_RETURN_ON_ERROR(epd_write_command(0x91), TAG, "cmd 0x91 failed");
    ESP_RETURN_ON_ERROR(
        epd_set_partial_ram_area(0, 0, EPD_213_Z19C_WIDTH, EPD_213_Z19C_HEIGHT),
        TAG,
        "set full area failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x10), TAG, "cmd 0x10 failed");
    ESP_RETURN_ON_ERROR(epd_write_repeat(black_value, EPD_213_Z19C_BUF_LEN), TAG, "write black buffer failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x13), TAG, "cmd 0x13 failed");
    ESP_RETURN_ON_ERROR(epd_write_repeat(color_value, EPD_213_Z19C_BUF_LEN), TAG, "write color buffer failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x92), TAG, "cmd 0x92 failed");
    return ESP_OK;
}

static esp_err_t epd_write_image_region(
    uint8_t command,
    const uint8_t *bitmap,
    int16_t x,
    int16_t y,
    int16_t w,
    int16_t h,
    bool invert)
{
    if (s_initial_write) {
        ESP_RETURN_ON_ERROR(epd_write_screen_buffer(0xFF, 0xFF), TAG, "initial buffer clean failed");
    }

    epd_delay_ms(1);

    int16_t wb = (int16_t)((w + 7) / 8);
    x -= x % 8;
    w = (int16_t)(wb * 8);

    int16_t x1 = x < 0 ? 0 : x;
    int16_t y1 = y < 0 ? 0 : y;
    int16_t w1 = x + w < (int16_t)EPD_213_Z19C_WIDTH ? w : (int16_t)EPD_213_Z19C_WIDTH - x;
    int16_t h1 = y + h < (int16_t)EPD_213_Z19C_HEIGHT ? h : (int16_t)EPD_213_Z19C_HEIGHT - y;
    int16_t dx = x1 - x;
    int16_t dy = y1 - y;

    w1 -= dx;
    h1 -= dy;

    if ((w1 <= 0) || (h1 <= 0)) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(epd_init_part(), TAG, "write image init failed");
    ESP_RETURN_ON_ERROR(epd_write_command(0x91), TAG, "cmd 0x91 failed");
    ESP_RETURN_ON_ERROR(
        epd_set_partial_ram_area((uint16_t)x1, (uint16_t)y1, (uint16_t)w1, (uint16_t)h1),
        TAG,
        "set image area failed");
    ESP_RETURN_ON_ERROR(epd_write_command(command), TAG, "image plane cmd failed");

    for (int16_t i = 0; i < h1; ++i) {
        for (int16_t j = 0; j < (w1 / 8); ++j) {
            uint8_t data = 0xFF;
            if (bitmap != NULL) {
                const int16_t idx = (int16_t)(j + dx / 8 + (i + dy) * wb);
                data = bitmap[idx];
                if (invert) {
                    data = (uint8_t)~data;
                }
            }
            ESP_RETURN_ON_ERROR(epd_write_data_byte(data), TAG, "image data failed");
        }
    }

    ESP_RETURN_ON_ERROR(epd_write_command(0x92), TAG, "cmd 0x92 failed");
    epd_delay_ms(1);
    return ESP_OK;
}

static esp_err_t epd_write_image_full(const uint8_t *black, const uint8_t *color)
{
    ESP_RETURN_ON_ERROR(
        epd_write_image_region(0x10, black, 0, 0, EPD_213_Z19C_WIDTH, EPD_213_Z19C_HEIGHT, false),
        TAG,
        "write black full failed");
    ESP_RETURN_ON_ERROR(
        epd_write_image_region(0x13, color, 0, 0, EPD_213_Z19C_WIDTH, EPD_213_Z19C_HEIGHT, false),
        TAG,
        "write color full failed");
    return ESP_OK;
}

esp_err_t epd_213_z19c_init(void)
{
    ESP_RETURN_ON_ERROR(epd_begin_session(), TAG, "begin session failed");
    return epd_init_part();
}

esp_err_t epd_213_z19c_clear_white(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "panel not ready");

    s_initial_write = false;
    ESP_RETURN_ON_ERROR(epd_init_part(), TAG, "clear init failed");
    ESP_RETURN_ON_ERROR(epd_write_command(0x91), TAG, "cmd 0x91 failed");
    ESP_RETURN_ON_ERROR(
        epd_set_partial_ram_area(0, 0, EPD_213_Z19C_WIDTH, EPD_213_Z19C_HEIGHT),
        TAG,
        "set clear area failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x10), TAG, "cmd 0x10 failed");
    ESP_RETURN_ON_ERROR(epd_write_repeat(0xFF, EPD_213_Z19C_BUF_LEN), TAG, "write clear 0x10 failed");

    ESP_RETURN_ON_ERROR(epd_write_command(0x13), TAG, "cmd 0x13 failed");
    ESP_RETURN_ON_ERROR(epd_write_repeat(0xFF, EPD_213_Z19C_BUF_LEN), TAG, "write clear 0x13 failed");

    ESP_RETURN_ON_ERROR(epd_update_part(), TAG, "clear update failed");
    ESP_RETURN_ON_ERROR(epd_write_command(0x92), TAG, "cmd 0x92 failed");
    return epd_power_off();
}

esp_err_t epd_213_z19c_clear_white_gx(void)
{
    return epd_213_z19c_clear_white();
}

esp_err_t epd_213_z19c_display_frame(const uint8_t *prev_plane, const uint8_t *new_plane)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "panel not ready");
    ESP_RETURN_ON_FALSE(prev_plane != NULL, ESP_ERR_INVALID_ARG, TAG, "prev plane null");
    ESP_RETURN_ON_FALSE(new_plane != NULL, ESP_ERR_INVALID_ARG, TAG, "new plane null");

    ESP_RETURN_ON_ERROR(epd_write_image_full(prev_plane, new_plane), TAG, "write image full failed");
    ESP_RETURN_ON_ERROR(epd_update_full(), TAG, "full update failed");
    return epd_power_off();
}

esp_err_t epd_213_z19c_display_frame_gx(const uint8_t *prev_plane, const uint8_t *new_plane, uint16_t page_height)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "panel not ready");
    ESP_RETURN_ON_FALSE(prev_plane != NULL, ESP_ERR_INVALID_ARG, TAG, "prev plane null");
    ESP_RETURN_ON_FALSE(new_plane != NULL, ESP_ERR_INVALID_ARG, TAG, "new plane null");

    if ((page_height == 0U) || (page_height > EPD_213_Z19C_HEIGHT)) {
        page_height = EPD_213_Z19C_HEIGHT;
    }

    for (uint16_t y = 0; y < EPD_213_Z19C_HEIGHT; y = (uint16_t)(y + page_height)) {
        const uint16_t h = (uint16_t)(((y + page_height) <= EPD_213_Z19C_HEIGHT)
            ? page_height
            : (EPD_213_Z19C_HEIGHT - y));
        const size_t stride = EPD_213_Z19C_WIDTH / 8U;
        const uint8_t *prev_ptr = prev_plane + ((size_t)y * stride);
        const uint8_t *new_ptr = new_plane + ((size_t)y * stride);

        ESP_RETURN_ON_ERROR(
            epd_write_image_region(0x10, prev_ptr, 0, (int16_t)y, EPD_213_Z19C_WIDTH, (int16_t)h, false),
            TAG,
            "gx write 0x10 failed");
        ESP_RETURN_ON_ERROR(
            epd_write_image_region(0x13, new_ptr, 0, (int16_t)y, EPD_213_Z19C_WIDTH, (int16_t)h, false),
            TAG,
            "gx write 0x13 failed");
    }

    ESP_RETURN_ON_ERROR(epd_update_full(), TAG, "gx full update failed");
    return epd_power_off();
}

esp_err_t epd_213_z19c_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "panel not ready");

    ESP_RETURN_ON_ERROR(epd_power_off(), TAG, "power off before sleep failed");
    ESP_RETURN_ON_ERROR(epd_write_command(0x07), TAG, "cmd 0x07 failed");
    ESP_RETURN_ON_ERROR(epd_write_data_byte(0xA5), TAG, "data 0xA5 failed");
    epd_delay_ms(10);
    return epd_power_4150b(false);
}

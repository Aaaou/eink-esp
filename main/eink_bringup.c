#include "eink_bringup.h"

#include "board_config.h"
#include "board_i2c.h"
#include "board_spi.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8574.h"
#include <string.h>

static const char *TAG = "eink_bringup";

#define EINK_FB_SIZE ((BOARD_EINK_WIDTH * BOARD_EINK_HEIGHT) / 8)

static uint8_t s_black_fb[EINK_FB_SIZE];
static uint8_t s_color_fb[EINK_FB_SIZE];

typedef struct {
    pcf8574_t iox;
    spi_device_handle_t spi;
} eink_context_t;

static esp_err_t eink_gpio_init(void)
{
    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_DC_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio_config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_EINK_DC_GPIO, 0), TAG, "set dc low failed");
    return ESP_OK;
}

static esp_err_t eink_spi_write(spi_device_handle_t spi, bool is_data, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(spi != NULL, ESP_ERR_INVALID_ARG, TAG, "spi is null");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is null");

    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_EINK_DC_GPIO, is_data ? 1 : 0), TAG, "set dc failed");

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };

    return spi_device_polling_transmit(spi, &t);
}

static esp_err_t eink_write_cmd(spi_device_handle_t spi, uint8_t cmd)
{
    return eink_spi_write(spi, false, &cmd, 1);
}

static esp_err_t eink_write_data(spi_device_handle_t spi, const uint8_t *data, size_t len)
{
    return eink_spi_write(spi, true, data, len);
}

static esp_err_t eink_ctrl_power(pcf8574_t *iox, bool on)
{
    ESP_LOGI(TAG, "4150B control via PCF8574 P2 -> %s", on ? "ON" : "OFF");
    return pcf8574_set_bit(iox, BOARD_IOX_BIT_EINK_CTRL_4150B, on);
}

static esp_err_t eink_reset_pulse(pcf8574_t *iox)
{
    ESP_LOGI(TAG, "Resetting e-paper via PCF8574 P1");
    ESP_RETURN_ON_ERROR(pcf8574_set_bit(iox, BOARD_IOX_BIT_EINK_RES, 0), TAG, "assert reset failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_EINK_RESET_HOLD_MS));
    ESP_RETURN_ON_ERROR(pcf8574_set_bit(iox, BOARD_IOX_BIT_EINK_RES, 1), TAG, "release reset failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_EINK_RESET_RELEASE_MS));
    return ESP_OK;
}

static esp_err_t eink_send_bootstrap_sequence(spi_device_handle_t spi)
{
    ESP_LOGI(TAG, "Sending conservative UC8151D-style bootstrap sequence (BUSY not connected)");

    const uint8_t soft_reset = 0x12;
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, soft_reset), TAG, "soft reset command failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_EINK_INIT_GUARD_MS));

    const uint8_t panel_setting_cmd = 0x00;
    const uint8_t panel_setting_data[] = { 0x0F };
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, panel_setting_cmd), TAG, "panel setting command failed");
    ESP_RETURN_ON_ERROR(eink_write_data(spi, panel_setting_data, sizeof(panel_setting_data)),
                        TAG, "panel setting data failed");

    const uint8_t resolution_cmd = 0x61;
    const uint8_t resolution_data[] = { 0x68, 0x00, 0xD4 };
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, resolution_cmd), TAG, "resolution command failed");
    ESP_RETURN_ON_ERROR(eink_write_data(spi, resolution_data, sizeof(resolution_data)),
                        TAG, "resolution data failed");

    const uint8_t vcom_cmd = 0x50;
    const uint8_t vcom_data[] = { 0x97 };
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, vcom_cmd), TAG, "vcom command failed");
    ESP_RETURN_ON_ERROR(eink_write_data(spi, vcom_data, sizeof(vcom_data)),
                        TAG, "vcom data failed");

    ESP_LOGI(TAG, "Bootstrap sequence dispatched. Hardware engineer should verify SPI waveform and panel rails.");
    return ESP_OK;
}

static void eink_fb_clear(uint8_t *fb, uint8_t value)
{
    memset(fb, value, EINK_FB_SIZE);
}

static void eink_fb_set_pixel(uint8_t *fb, int x, int y, bool colored)
{
    if ((x < 0) || (x >= BOARD_EINK_WIDTH) || (y < 0) || (y >= BOARD_EINK_HEIGHT)) {
        return;
    }

    const int stride = BOARD_EINK_WIDTH / 8;
    const size_t index = (size_t)y * (size_t)stride + (size_t)(x / 8);
    const uint8_t mask = (uint8_t)(0x80U >> (x % 8));

    if (colored) {
        fb[index] &= (uint8_t)~mask;
    } else {
        fb[index] |= mask;
    }
}

static void eink_fb_hline(uint8_t *fb, int x, int y, int w, bool colored)
{
    for (int i = 0; i < w; ++i) {
        eink_fb_set_pixel(fb, x + i, y, colored);
    }
}

static void eink_fb_vline(uint8_t *fb, int x, int y, int h, bool colored)
{
    for (int i = 0; i < h; ++i) {
        eink_fb_set_pixel(fb, x, y + i, colored);
    }
}

static void eink_fb_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool colored)
{
    for (int row = 0; row < h; ++row) {
        eink_fb_hline(fb, x, y + row, w, colored);
    }
}

static void eink_build_test_label(void)
{
    const int label_x = 12;
    const int label_y = 72;
    const int label_w = 80;
    const int label_h = 56;

    eink_fb_clear(s_black_fb, 0xFF);
    eink_fb_clear(s_color_fb, 0xFF);

    // Outer black frame
    eink_fb_hline(s_black_fb, label_x, label_y, label_w, true);
    eink_fb_hline(s_black_fb, label_x, label_y + label_h - 1, label_w, true);
    eink_fb_vline(s_black_fb, label_x, label_y, label_h, true);
    eink_fb_vline(s_black_fb, label_x + label_w - 1, label_y, label_h, true);

    // Red header stripe to verify the color plane independently.
    eink_fb_fill_rect(s_color_fb, label_x + 4, label_y + 4, label_w - 8, 10, true);

    // Black content blocks and alignment marks.
    eink_fb_fill_rect(s_black_fb, label_x + 8, label_y + 22, 18, 18, true);
    eink_fb_fill_rect(s_black_fb, label_x + 32, label_y + 22, 6, 18, true);
    eink_fb_fill_rect(s_black_fb, label_x + 44, label_y + 22, 28, 6, true);
    eink_fb_fill_rect(s_black_fb, label_x + 44, label_y + 34, 28, 6, true);
    eink_fb_hline(s_black_fb, 0, 0, 10, true);
    eink_fb_vline(s_black_fb, 0, 0, 10, true);
    eink_fb_hline(s_black_fb, BOARD_EINK_WIDTH - 10, BOARD_EINK_HEIGHT - 1, 10, true);
    eink_fb_vline(s_black_fb, BOARD_EINK_WIDTH - 1, BOARD_EINK_HEIGHT - 10, 10, true);
}

static esp_err_t eink_write_full_frame(spi_device_handle_t spi, uint8_t command, const uint8_t *fb)
{
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, command), TAG, "frame command failed");
    return eink_write_data(spi, fb, EINK_FB_SIZE);
}

static esp_err_t eink_refresh_and_wait(spi_device_handle_t spi)
{
    const uint8_t power_on_cmd = 0x04;
    const uint8_t refresh_cmd = 0x12;
    const uint8_t power_off_cmd = 0x02;

    ESP_LOGI(TAG, "Powering on panel for test pattern");
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, power_on_cmd), TAG, "power on command failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Refreshing test pattern; BUSY is not connected so waiting 17 seconds conservatively");
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, refresh_cmd), TAG, "refresh command failed");
    vTaskDelay(pdMS_TO_TICKS(17000));

    ESP_LOGI(TAG, "Powering off panel after test refresh");
    ESP_RETURN_ON_ERROR(eink_write_cmd(spi, power_off_cmd), TAG, "power off command failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t eink_draw_test_label(spi_device_handle_t spi)
{
    ESP_LOGI(TAG, "Building visible test label: white background, black frame, red stripe");
    eink_build_test_label();

    // First-pass Z19c-compatible mapping:
    // 0x10 writes the black/white plane and 0x13 writes the chromatic plane.
    ESP_RETURN_ON_ERROR(eink_write_full_frame(spi, 0x10, s_black_fb), TAG, "black plane write failed");
    ESP_RETURN_ON_ERROR(eink_write_full_frame(spi, 0x13, s_color_fb), TAG, "color plane write failed");
    ESP_RETURN_ON_ERROR(eink_refresh_and_wait(spi), TAG, "panel refresh failed");

    ESP_LOGI(TAG, "If the mapping is correct, the panel should show a centered black-framed tag with a red top stripe.");
    return ESP_OK;
}

static esp_err_t eink_prepare_expander(eink_context_t *ctx)
{
    const uint8_t initial_latch = 0xFF;
    ESP_RETURN_ON_ERROR(pcf8574_init(&ctx->iox, BOARD_IOX_ADDR, initial_latch), TAG, "pcf8574 init failed");
    ESP_RETURN_ON_ERROR(eink_ctrl_power(&ctx->iox, true), TAG, "power enable failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_EINK_POWER_SETTLE_MS));
    ESP_RETURN_ON_ERROR(eink_reset_pulse(&ctx->iox), TAG, "reset pulse failed");
    return ESP_OK;
}

esp_err_t eink_bringup_run(void)
{
    eink_context_t ctx = { 0 };

    ESP_LOGI(TAG, "Starting e-paper bring-up for 2.13-inch tri-color panel");
    ESP_LOGI(TAG, "Board assumptions: 104x212, MOSI=GPIO6(CSDI), SCLK=GPIO7, CS=GPIO8, DC=GPIO10, RES=PCF8574.P1, CTRL=PCF8574.P2, BUSY=NC");

    ESP_RETURN_ON_ERROR(eink_gpio_init(), TAG, "eink gpio init failed");
    ESP_RETURN_ON_ERROR(eink_prepare_expander(&ctx), TAG, "eink expander prep failed");
    ESP_RETURN_ON_ERROR(board_eink_spi_init(&ctx.spi), TAG, "eink spi init failed");
    ESP_RETURN_ON_ERROR(eink_send_bootstrap_sequence(ctx.spi), TAG, "bootstrap sequence failed");
    ESP_RETURN_ON_ERROR(eink_draw_test_label(ctx.spi), TAG, "test label failed");

    ESP_LOGW(TAG, "esp_epaper component is declared in idf_component.yml, but this project currently uses a raw bring-up path until the custom 104x212 tri-color panel is fully integrated.");
    ESP_LOGW(TAG, "Because BUSY is not connected, refresh timing currently relies on fixed delays.");
    return ESP_OK;
}

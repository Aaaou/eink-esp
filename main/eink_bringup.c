#include "eink_bringup.h"

#include "epd_213_z19c.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "eink_bringup";

#define EINK_LANDSCAPE_WIDTH  EPD_213_Z19C_HEIGHT
#define EINK_LANDSCAPE_HEIGHT EPD_213_Z19C_WIDTH
#define EINK_GX_PAGE_HEIGHT   EPD_213_Z19C_HEIGHT

static uint8_t s_prev_plane[EPD_213_Z19C_BUF_LEN];
static uint8_t s_new_plane[EPD_213_Z19C_BUF_LEN];

static void fb_clear(uint8_t *fb, uint8_t value)
{
    memset(fb, value, EPD_213_Z19C_BUF_LEN);
}

static void fb_set_native_pixel(uint8_t *fb, int x, int y, bool colored)
{
    if ((x < 0) || (x >= EPD_213_Z19C_WIDTH) || (y < 0) || (y >= EPD_213_Z19C_HEIGHT)) {
        return;
    }

    const int stride = EPD_213_Z19C_WIDTH / 8;
    const size_t index = (size_t)y * (size_t)stride + (size_t)(x / 8);
    const uint8_t mask = (uint8_t)(0x80U >> (x % 8));

    if (colored) {
        fb[index] &= (uint8_t)~mask;
    } else {
        fb[index] |= mask;
    }
}

static void fb_set_rot1_pixel(uint8_t *fb, int x, int y, bool colored)
{
    if ((x < 0) || (x >= EINK_LANDSCAPE_WIDTH) || (y < 0) || (y >= EINK_LANDSCAPE_HEIGHT)) {
        return;
    }

    const int native_x = EPD_213_Z19C_WIDTH - 1 - y;
    const int native_y = x;
    fb_set_native_pixel(fb, native_x, native_y, colored);
}

static void fb_hline_rot1(uint8_t *fb, int x, int y, int w, bool colored)
{
    for (int i = 0; i < w; ++i) {
        fb_set_rot1_pixel(fb, x + i, y, colored);
    }
}

static void fb_vline_rot1(uint8_t *fb, int x, int y, int h, bool colored)
{
    for (int i = 0; i < h; ++i) {
        fb_set_rot1_pixel(fb, x, y + i, colored);
    }
}

static void fb_fill_rect_rot1(uint8_t *fb, int x, int y, int w, int h, bool colored)
{
    for (int row = 0; row < h; ++row) {
        fb_hline_rot1(fb, x, y + row, w, colored);
    }
}

static void build_test_pattern(void)
{
    fb_clear(s_prev_plane, 0xFF);
    fb_clear(s_new_plane, 0xFF);

    fb_hline_rot1(s_prev_plane, 5, 5, EINK_LANDSCAPE_WIDTH - 10, true);
    fb_hline_rot1(s_prev_plane, 5, EINK_LANDSCAPE_HEIGHT - 6, EINK_LANDSCAPE_WIDTH - 10, true);
    fb_vline_rot1(s_prev_plane, 5, 5, EINK_LANDSCAPE_HEIGHT - 10, true);
    fb_vline_rot1(s_prev_plane, EINK_LANDSCAPE_WIDTH - 6, 5, EINK_LANDSCAPE_HEIGHT - 10, true);

    fb_hline_rot1(s_new_plane, 9, 9, EINK_LANDSCAPE_WIDTH - 18, true);
    fb_hline_rot1(s_new_plane, 9, EINK_LANDSCAPE_HEIGHT - 10, EINK_LANDSCAPE_WIDTH - 18, true);
    fb_vline_rot1(s_new_plane, 9, 9, EINK_LANDSCAPE_HEIGHT - 18, true);
    fb_vline_rot1(s_new_plane, EINK_LANDSCAPE_WIDTH - 10, 9, EINK_LANDSCAPE_HEIGHT - 18, true);

    fb_fill_rect_rot1(s_new_plane, 18, 18, 110, 14, true);
    fb_fill_rect_rot1(s_prev_plane, 20, 42, 70, 10, true);
    fb_fill_rect_rot1(s_new_plane, 20, 60, 100, 10, true);
    fb_fill_rect_rot1(s_prev_plane, EINK_LANDSCAPE_WIDTH - 54, 34, 28, 28, true);
}

esp_err_t eink_bringup_run(void)
{
    ESP_LOGI(TAG, "==== 墨水屏刷新验证 ====");
    ESP_LOGI(TAG, "[√] 接线确认: MOSI=GPIO6 SCLK=GPIO7 CS=GPIO8 DC=GPIO10 RES=P1 4150B_EN=P2 BUSY=未接");
    ESP_LOGI(TAG, "[√] 当前策略: 对齐 GxEPD2_3C + GxEPD2_213_Z19c 官方流程");
    ESP_LOGI(TAG, "[√] BUSY 处理: Arduino 主程序传入 EPD_BUSY=-1，底层不读忙脚，只按官方固定时间等待");
    ESP_LOGI(TAG, "[√] 当前 page_height=%u (与 Arduino 示例 HEIGHT 模板一致)", (unsigned)EINK_GX_PAGE_HEIGHT);

    ESP_RETURN_ON_ERROR(epd_213_z19c_init(), TAG, "epd_213_z19c_init failed");
    ESP_LOGI(TAG, "[√] 底层初始化完成");

    ESP_LOGI(TAG, "---- 阶段1: 官方 clearScreen 风格写白 ----");
    ESP_RETURN_ON_ERROR(epd_213_z19c_clear_white_gx(), TAG, "clear white gx failed");
    ESP_LOGI(TAG, "[√] 阶段1命令发送完成");

    ESP_LOGI(TAG, "---- 阶段2: 官方 firstPage/nextPage 风格写图 ----");
    build_test_pattern();
    ESP_RETURN_ON_ERROR(
        epd_213_z19c_display_frame_gx(s_prev_plane, s_new_plane, EINK_GX_PAGE_HEIGHT),
        TAG,
        "display frame gx failed");
    ESP_LOGI(TAG, "[√] 阶段2命令发送完成");

    ESP_LOGI(TAG, "预期现象:");
    ESP_LOGI(TAG, "1. 阶段1 应尽量清掉旧图");
    ESP_LOGI(TAG, "2. 阶段2 应显示横屏外黑框、内红框和黑色块状标签");
    ESP_LOGW(TAG, "[!] 若仍保持旧图，下一步优先怀疑屏参差异或仍缺少官方更底层初始化细节");
    return ESP_OK;
}

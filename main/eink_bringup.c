#include "eink_bringup.h"

#include "eink_panel.h"
#include "esp_check.h"
#include "esp_log.h"
#include "project_defaults.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "eink_bringup";

#define EINK_LANDSCAPE_WIDTH  EINK_PANEL_HEIGHT
#define EINK_LANDSCAPE_HEIGHT EINK_PANEL_WIDTH
#define EINK_GX_PAGE_HEIGHT   EINK_PANEL_HEIGHT

static uint8_t s_prev_plane[EINK_PANEL_BUF_LEN];
static uint8_t s_new_plane[EINK_PANEL_BUF_LEN];

static void fb_clear(uint8_t *fb, uint8_t value)
{
    memset(fb, value, EINK_PANEL_BUF_LEN);
}

static void fb_set_native_pixel(uint8_t *fb, int x, int y, bool colored)
{
    if ((x < 0) || (x >= EINK_PANEL_WIDTH) || (y < 0) || (y >= EINK_PANEL_HEIGHT)) {
        return;
    }

    const int stride = EINK_PANEL_WIDTH / 8;
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

    const int native_x = EINK_PANEL_WIDTH - 1 - y;
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
    const eink_panel_descriptor_t *panel = eink_panel_get_descriptor();

    ESP_LOGI(TAG, "==== E-ink refresh validation ====");
    ESP_LOGI(TAG, "[√] Wiring: MOSI=GPIO6 SCLK=GPIO7 CS=GPIO8 DC=GPIO10 RES=P1 4150B_EN=P2 BUSY=NC");
    ESP_LOGI(TAG, "[√] Panel: %s controller=%s ref=%s",
        panel->model_id,
        panel->controller,
        panel->reference_driver);
    ESP_LOGI(TAG, "[√] Timing: power_on=%ums clear=%ums full=%ums power_off=%ums",
        panel->power_on_time_ms,
        panel->clear_refresh_time_ms,
        panel->full_refresh_time_ms,
        panel->power_off_time_ms);
    ESP_LOGI(TAG, "[√] Page height: %u", (unsigned)EINK_GX_PAGE_HEIGHT);

    ESP_RETURN_ON_ERROR(eink_panel_init(), TAG, "eink_panel_init failed");
    ESP_LOGI(TAG, "[√] Panel low-level init complete");

    if (CONFIG_INK_PANEL_CLEAR_BEFORE_DRAW) {
        ESP_LOGI(TAG, "---- Stage 1: clear white ----");
        ESP_RETURN_ON_ERROR(eink_panel_clear_white_gx(), TAG, "clear white gx failed");
        ESP_LOGI(TAG, "[√] Stage 1 commands sent");
    } else {
        ESP_LOGI(TAG, "[√] Stage 1 clear skipped by config");
    }

    ESP_LOGI(TAG, "---- Stage 2: draw test pattern ----");
    build_test_pattern();
    ESP_RETURN_ON_ERROR(
        eink_panel_display_frame_gx(s_prev_plane, s_new_plane, EINK_GX_PAGE_HEIGHT),
        TAG,
        "display frame gx failed");
    ESP_LOGI(TAG, "[√] Stage 2 commands sent");

    ESP_LOGI(TAG, "Expected result:");
    ESP_LOGI(TAG, "1. Stage 2 should show outer black frame, inner red frame, and black blocks");
    if (CONFIG_INK_PANEL_CLEAR_BEFORE_DRAW) {
        ESP_LOGI(TAG, "2. Stage 1 should clear old content toward white before drawing");
    }
    ESP_LOGW(TAG, "[!] If the old image remains, check panel variant differences or deeper init details");
    return ESP_OK;
}

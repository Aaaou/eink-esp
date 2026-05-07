#include "eink_scene.h"

#include "eink_panel.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"
#include "font/fmt_txt/lv_font_fmt_txt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    char ch;
    uint8_t rows[7];
} glyph_t;

static uint8_t s_bw[EINK_PANEL_BUF_LEN];
static uint8_t s_red[EINK_PANEL_BUF_LEN];

#if CONFIG_INK_SCENE_CJK_FONT_PUHUI_14
LV_FONT_DECLARE(font_puhui_14_1);
#elif CONFIG_INK_SCENE_CJK_FONT_SOURCE_HAN_14
LV_FONT_DECLARE(lv_font_source_han_sans_sc_14_cjk);
#endif

static const char *TAG = "eink_scene";

static const uint8_t s_opa4_table[16] = {
    0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255,
};

static const glyph_t s_font[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

static const char *s_week_name[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *s_lunar_month_name[] = {
    "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "L10", "L11", "L12",
};

static const glyph_t *find_glyph(char c)
{
    size_t i;
    char upper = (char)toupper((unsigned char)c);

    for (i = 0; i < sizeof(s_font) / sizeof(s_font[0]); ++i) {
        if (s_font[i].ch == upper) {
            return &s_font[i];
        }
    }
    return &s_font[0];
}

static void clear_layer(uint8_t *buf)
{
    memset(buf, 0xFF, EINK_PANEL_BUF_LEN);
}

static void set_pixel(uint8_t *buf, int x, int y, bool colored)
{
    const int stride = (int)EINK_PANEL_WIDTH / 8;
    size_t index;
    uint8_t mask;

    if ((x < 0) || (x >= (int)EINK_PANEL_WIDTH) || (y < 0) || (y >= (int)EINK_PANEL_HEIGHT)) {
        return;
    }

    index = (size_t)y * (size_t)stride + (size_t)(x / 8);
    mask = (uint8_t)(0x80U >> (x % 8));
    if (colored) {
        buf[index] &= (uint8_t)~mask;
    } else {
        buf[index] |= mask;
    }
}

static void set_landscape_pixel(uint8_t *buf, int x, int y, bool colored)
{
    if ((x < 0) || (x >= (int)EINK_PANEL_HEIGHT) || (y < 0) || (y >= (int)EINK_PANEL_WIDTH)) {
        return;
    }
    set_pixel(buf, y, ((int)EINK_PANEL_HEIGHT - 1) - x, colored);
}

static void draw_landscape_hline(uint8_t *buf, int x, int y, int w, bool colored)
{
    int ix;
    for (ix = x; ix < x + w; ++ix) {
        set_landscape_pixel(buf, ix, y, colored);
    }
}

static void draw_landscape_vline(uint8_t *buf, int x, int y, int h, bool colored)
{
    int iy;
    for (iy = y; iy < y + h; ++iy) {
        set_landscape_pixel(buf, x, iy, colored);
    }
}

static void draw_landscape_rect(uint8_t *buf, int x, int y, int w, int h, bool colored)
{
    draw_landscape_hline(buf, x, y, w, colored);
    draw_landscape_hline(buf, x, y + h - 1, w, colored);
    draw_landscape_vline(buf, x, y, h, colored);
    draw_landscape_vline(buf, x + w - 1, y, h, colored);
}

static void fill_landscape_rect(uint8_t *buf, int x, int y, int w, int h, bool colored)
{
    int iy;
    for (iy = y; iy < y + h; ++iy) {
        draw_landscape_hline(buf, x, iy, w, colored);
    }
}

static void draw_landscape_circle(uint8_t *buf, int cx, int cy, int radius, bool colored)
{
    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        set_landscape_pixel(buf, cx + x, cy + y, colored);
        set_landscape_pixel(buf, cx + y, cy + x, colored);
        set_landscape_pixel(buf, cx - y, cy + x, colored);
        set_landscape_pixel(buf, cx - x, cy + y, colored);
        set_landscape_pixel(buf, cx - x, cy - y, colored);
        set_landscape_pixel(buf, cx - y, cy - x, colored);
        set_landscape_pixel(buf, cx + y, cy - x, colored);
        set_landscape_pixel(buf, cx + x, cy - y, colored);

        if (err <= 0) {
            y++;
            err += (2 * y) + 1;
        }
        if (err > 0) {
            x--;
            err -= (2 * x) + 1;
        }
    }
}

static void fill_landscape_circle(uint8_t *buf, int cx, int cy, int radius, bool colored)
{
    int x;
    int y;
    const int rr = radius * radius;

    for (y = -radius; y <= radius; ++y) {
        for (x = -radius; x <= radius; ++x) {
            if ((x * x) + (y * y) <= rr) {
                set_landscape_pixel(buf, cx + x, cy + y, colored);
            }
        }
    }
}

static void draw_landscape_char_scaled(uint8_t *buf, int x, int y, char c, int scale, bool colored)
{
    int row;
    int col;
    int sx;
    int sy;
    const glyph_t *glyph = find_glyph(c);

    for (row = 0; row < 7; ++row) {
        for (col = 0; col < 5; ++col) {
            if ((glyph->rows[row] >> (4 - col)) & 0x01) {
                for (sy = 0; sy < scale; ++sy) {
                    for (sx = 0; sx < scale; ++sx) {
                        set_landscape_pixel(buf, x + (col * scale) + sx, y + (row * scale) + sy, colored);
                    }
                }
            }
        }
    }
}

static int text_width(const char *text, int scale)
{
    const int len = (int)strlen(text);
    if (len <= 0) {
        return 0;
    }
    return (len * (5 * scale)) + ((len - 1) * scale);
}

static void draw_landscape_text(uint8_t *buf, int x, int y, const char *text, int scale, bool colored)
{
    int cursor_x = x;
    size_t i;

    for (i = 0; text[i] != '\0'; ++i) {
        draw_landscape_char_scaled(buf, cursor_x, y, text[i], scale, colored);
        cursor_x += (5 * scale) + scale;
    }
}

static void draw_landscape_text_center(uint8_t *buf, int center_x, int y, const char *text, int scale, bool colored)
{
    draw_landscape_text(buf, center_x - (text_width(text, scale) / 2), y, text, scale, colored);
}

static uint32_t utf8_next_codepoint(const char **cursor)
{
    const uint8_t *s = (const uint8_t *)*cursor;
    uint32_t cp;

    if (s[0] == '\0') {
        return 0;
    }
    if (s[0] < 0x80) {
        *cursor += 1;
        return s[0];
    }
    if (((s[0] & 0xE0) == 0xC0) && ((s[1] & 0xC0) == 0x80)) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *cursor += 2;
        return cp;
    }
    if (((s[0] & 0xF0) == 0xE0) && ((s[1] & 0xC0) == 0x80) && ((s[2] & 0xC0) == 0x80)) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *cursor += 3;
        return cp;
    }
    if (((s[0] & 0xF8) == 0xF0) && ((s[1] & 0xC0) == 0x80) && ((s[2] & 0xC0) == 0x80) && ((s[3] & 0xC0) == 0x80)) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *cursor += 4;
        return cp;
    }

    *cursor += 1;
    return '?';
}

static int draw_landscape_lvgl_glyph(uint8_t *buf, int x, int baseline_y, uint32_t codepoint, bool colored)
{
#if CONFIG_INK_SCENE_CJK_FONT_PUHUI_14
    const lv_font_t *font = &font_puhui_14_1;
#elif CONFIG_INK_SCENE_CJK_FONT_SOURCE_HAN_14
    const lv_font_t *font = &lv_font_source_han_sans_sc_14_cjk;
#else
#error "Select a CJK scene font"
#endif
    LV_ATTRIBUTE_MEM_ALIGN static uint8_t glyph_a8[32 * 32];
    lv_font_glyph_dsc_t dsc;
    const lv_font_fmt_txt_dsc_t *font_dsc = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t *glyph_dsc;
    const uint8_t *bitmap_in;
    int draw_x;
    int draw_y;
    int advance;
    uint32_t stride_in;
    uint32_t stride_out;
    uint32_t data_size;
    int bit_index = 0;

    if (!lv_font_get_glyph_dsc(font, &dsc, codepoint, 0)) {
        ESP_LOGW(TAG, "missing glyph U+%04" PRIX32, codepoint);
        codepoint = '?';
        if (!lv_font_get_glyph_dsc(font, &dsc, codepoint, 0)) {
            return 8;
        }
    }

    advance = (int)dsc.adv_w;
    if ((dsc.box_w == 0) || (dsc.box_h == 0)) {
        return advance > 0 ? advance : 4;
    }

    if (dsc.is_placeholder) {
        ESP_LOGW(TAG, "placeholder glyph U+%04" PRIX32, codepoint);
    }

    if ((font_dsc == NULL) || (dsc.gid.index == 0) || (font_dsc->bitmap_format != LV_FONT_FMT_TXT_PLAIN) ||
        ((font_dsc->bpp != 1) && (font_dsc->bpp != 4))) {
        ESP_LOGW(TAG, "unsupported glyph U+%04" PRIX32 " format", codepoint);
        return advance > 0 ? advance : (int)dsc.box_w;
    }

    glyph_dsc = &font_dsc->glyph_dsc[dsc.gid.index];
    bitmap_in = &font_dsc->glyph_bitmap[glyph_dsc->bitmap_index];
    stride_in = dsc.stride != 0 ? dsc.stride : (((uint32_t)dsc.box_w * font_dsc->bpp + 7U) / 8U);
    stride_out = dsc.box_w;
    data_size = stride_out * dsc.box_h;
    if (data_size > sizeof(glyph_a8)) {
        ESP_LOGW(TAG, "glyph U+%04" PRIX32 " too large: %ux%u", codepoint, dsc.box_w, dsc.box_h);
        return advance > 0 ? advance : (int)dsc.box_w;
    }
    memset(glyph_a8, 0, data_size);

    draw_x = x + dsc.ofs_x;
    draw_y = baseline_y - dsc.box_h - dsc.ofs_y;

    for (int gy = 0; gy < dsc.box_h; ++gy) {
        uint32_t line_rem = stride_in;
        uint8_t *row = glyph_a8 + ((uint32_t)gy * stride_out);

        for (int gx = 0; gx < dsc.box_w; ++gx) {
            if (font_dsc->bpp == 1) {
                row[gx] = ((*bitmap_in) & (0x80U >> bit_index)) ? 255 : 0;
                if (bit_index == 7) {
                    if (line_rem > 0) {
                        line_rem--;
                    }
                    bitmap_in++;
                    bit_index = 0;
                } else {
                    bit_index++;
                }
            } else {
                if ((bit_index & 0x01) == 0) {
                    row[gx] = s_opa4_table[*bitmap_in >> 4];
                } else {
                    row[gx] = s_opa4_table[*bitmap_in & 0x0F];
                    if (line_rem > 0) {
                        line_rem--;
                    }
                    bitmap_in++;
                }
                bit_index = (bit_index + 1) & 0x01;
            }

            if (row[gx] >= 96) {
                set_landscape_pixel(buf, draw_x + gx, draw_y + gy, colored);
            }
        }

        if (dsc.stride != 0) {
            bit_index = 0;
            bitmap_in += line_rem;
        }
    }

    lv_font_glyph_release_draw_data(&dsc);
    return advance > 0 ? advance : (int)dsc.box_w;
}

static void draw_landscape_utf8_text(uint8_t *buf, int x, int baseline_y, int max_w, const char *text, bool colored)
{
    int cursor_x = x;
    const char *cursor = text;

    if (text == NULL) {
        return;
    }

    while (*cursor != '\0') {
        const char *before = cursor;
        uint32_t cp = utf8_next_codepoint(&cursor);
        int advance;

        if (cp == 0) {
            break;
        }
        if ((cp == '\r') || (cp == '\n')) {
            break;
        }

        advance = draw_landscape_lvgl_glyph(buf, cursor_x, baseline_y, cp, colored);
        if ((cursor_x + advance) > (x + max_w)) {
            cursor = before;
            break;
        }
        cursor_x += advance;
    }
}

static void prepare_canvas(void)
{
    clear_layer(s_bw);
    clear_layer(s_red);
}

static int days_since_2000_01_01(const struct tm *value)
{
    struct tm base = {
        .tm_year = 100,
        .tm_mon = 0,
        .tm_mday = 1,
        .tm_hour = 12,
    };
    struct tm copy = *value;
    time_t base_time = mktime(&base);
    time_t value_time;

    copy.tm_hour = 12;
    copy.tm_min = 0;
    copy.tm_sec = 0;
    value_time = mktime(&copy);
    return (int)((value_time - base_time) / (24 * 60 * 60));
}

static void pseudo_lunar_text(const struct tm *value, char *buf, size_t buf_size)
{
    const int days = days_since_2000_01_01(value);
    int lunar_month = ((days / 29) % 12);
    int lunar_day = (days % 29) + 1;

    if (lunar_month < 0) {
        lunar_month += 12;
    }
    if (lunar_day < 1) {
        lunar_day += 29;
    }
    snprintf(buf, buf_size, "%s.%02d", s_lunar_month_name[lunar_month], lunar_day);
}

static void draw_landscape_day_card(const struct tm *value, int x, int y, int w, int h, bool main_card)
{
    char text[24];
    char lunar[16];
    const int center_x = x + (w / 2);

    if (main_card) {
        fill_landscape_rect(s_red, x, y, w, h, true);
        draw_landscape_rect(s_bw, x, y, w, h, true);
        snprintf(text, sizeof(text), "%02d", value->tm_mday);
        draw_landscape_text_center(s_red, center_x, y + 16, text, 5, false);
        snprintf(text, sizeof(text), "%02d/%02d", value->tm_mon + 1, value->tm_mday);
        draw_landscape_text_center(s_bw, center_x, y + 58, text, 1, true);
        draw_landscape_text_center(s_bw, center_x, y + 74, s_week_name[value->tm_wday], 1, true);
        pseudo_lunar_text(value, lunar, sizeof(lunar));
        draw_landscape_text_center(s_bw, center_x, y + 88, lunar, 1, true);
    } else {
        draw_landscape_rect(s_bw, x, y, w, h, true);
        snprintf(text, sizeof(text), "%02d", value->tm_mday);
        draw_landscape_text_center(s_bw, center_x, y + 14, text, 3, true);
        draw_landscape_text_center(s_bw, center_x, y + 48, s_week_name[value->tm_wday], 1, true);
        pseudo_lunar_text(value, lunar, sizeof(lunar));
        draw_landscape_text_center(s_red, center_x, y + 66, lunar, 1, true);
    }
}

esp_err_t eink_scene_show_frame(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    ESP_RETURN_ON_FALSE(bw_plane != NULL, ESP_ERR_INVALID_ARG, "eink_scene", "bw plane is null");
    ESP_RETURN_ON_FALSE(red_plane != NULL, ESP_ERR_INVALID_ARG, "eink_scene", "red plane is null");

    ESP_RETURN_ON_ERROR(eink_panel_init(), "eink_scene", "panel init failed");
    ESP_RETURN_ON_ERROR(
        eink_panel_display_frame_gx(bw_plane, red_plane, EINK_PANEL_HEIGHT),
        "eink_scene",
        "display frame failed");
    return eink_panel_sleep();
}

esp_err_t eink_scene_show_calendar(time_t timestamp)
{
    char header[32];
    struct tm now_tm;
    struct tm prev_tm;
    struct tm next_tm;
    time_t prev_time;
    time_t next_time;

    if (timestamp <= 0) {
        timestamp = time(NULL);
    }
    localtime_r(&timestamp, &now_tm);
    prev_time = timestamp - (24 * 60 * 60);
    next_time = timestamp + (24 * 60 * 60);
    localtime_r(&prev_time, &prev_tm);
    localtime_r(&next_time, &next_tm);

    prepare_canvas();

    snprintf(header, sizeof(header), "%04d-%02d", now_tm.tm_year + 1900, now_tm.tm_mon + 1);
    draw_landscape_text_center(s_bw, 106, 4, header, 2, true);
    draw_landscape_hline(s_bw, 8, 26, 196, true);

    draw_landscape_day_card(&prev_tm, 4, 34, 54, 66, false);
    draw_landscape_day_card(&now_tm, 62, 30, 88, 72, true);
    draw_landscape_day_card(&next_tm, 154, 34, 54, 66, false);

    return eink_scene_show_frame(s_bw, s_red);
}

esp_err_t eink_scene_show_memos(const eink_memo_item_t *items, size_t item_count)
{
    const size_t count = (item_count > EINK_MEMO_MAX_ITEMS) ? EINK_MEMO_MAX_ITEMS : item_count;
    size_t i;

    ESP_RETURN_ON_FALSE(items != NULL, ESP_ERR_INVALID_ARG, "eink_scene", "memo items null");
    ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_INVALID_ARG, "eink_scene", "memo items empty");

    prepare_canvas();
    draw_landscape_text_center(s_bw, 106, 4, "MEMO", 2, true);
    draw_landscape_hline(s_bw, 8, 26, 196, true);

    for (i = 0; i < count; ++i) {
        const int y = 33 + ((int)i * 23);
        char index_text[4];

        snprintf(index_text, sizeof(index_text), "%u", (unsigned int)(i + 1));
        draw_landscape_rect(s_bw, 6, y, 18, 18, true);
        draw_landscape_text_center(s_bw, 15, y + 4, index_text, 1, true);

        draw_landscape_utf8_text(s_bw, 30, y + 15, 140, items[i].text[0] != '\0' ? items[i].text : "-", true);

        draw_landscape_circle(s_bw, 191, y + 9, 8, true);
        draw_landscape_circle(s_bw, 191, y + 9, 7, true);
        if (items[i].checked) {
            fill_landscape_circle(s_red, 191, y + 9, 5, true);
            draw_landscape_circle(s_bw, 191, y + 9, 3, false);
        } else {
            fill_landscape_circle(s_bw, 191, y + 9, 5, false);
        }
    }

    return eink_scene_show_frame(s_bw, s_red);
}

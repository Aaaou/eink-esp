#include "eink_scene_legacy.h"

#include "eink_epaper_display.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <esp_check.h>

typedef struct {
    char ch;
    uint8_t rows[7];
} glyph_t;

static EinkEpaperDisplay* s_display = nullptr;

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

static const char* s_week_name[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char* s_lunar_month_name[] = {"L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "L10", "L11", "L12"};

static const glyph_t* find_glyph(char c) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& glyph : s_font) {
        if (glyph.ch == upper) {
            return &glyph;
        }
    }
    return &s_font[0];
}

static int text_width(const char* text, int scale) {
    int len = text ? static_cast<int>(std::strlen(text)) : 0;
    return len > 0 ? (len * (5 * scale)) + ((len - 1) * scale) : 0;
}

static void draw_text_center(int center_x, int y, const char* text, int scale) {
    int cursor_x = center_x - (text_width(text, scale) / 2);
    if (text == nullptr || s_display == nullptr) {
        return;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const auto* glyph = find_glyph(text[i]);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph->rows[row] >> (4 - col)) & 0x01) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            s_display->SetPixel(cursor_x + col * scale + sx, y + row * scale + sy, true);
                        }
                    }
                }
            }
        }
        cursor_x += (5 * scale) + scale;
    }
}

static void fill_rect(int x, int y, int w, int h, bool black) {
    if (s_display == nullptr) {
        return;
    }
    for (int iy = y; iy < y + h; ++iy) {
        for (int ix = x; ix < x + w; ++ix) {
            s_display->SetPixel(ix, iy, black);
        }
    }
}

static int days_since_2000_01_01(const struct tm* value) {
    struct tm base = {};
    base.tm_year = 100;
    base.tm_mon = 0;
    base.tm_mday = 1;
    base.tm_hour = 12;
    struct tm copy = *value;
    time_t base_time = mktime(&base);
    copy.tm_hour = 12;
    copy.tm_min = 0;
    copy.tm_sec = 0;
    time_t value_time = mktime(&copy);
    return static_cast<int>((value_time - base_time) / (24 * 60 * 60));
}

static void pseudo_lunar_text(const struct tm* value, char* buf, size_t buf_size) {
    const int days = days_since_2000_01_01(value);
    int lunar_month = ((days / 29) % 12);
    int lunar_day = (days % 29) + 1;

    if (lunar_month < 0) {
        lunar_month += 12;
    }
    if (lunar_day < 1) {
        lunar_day += 29;
    }
    std::snprintf(buf, buf_size, "%s.%02d", s_lunar_month_name[lunar_month], lunar_day);
}

void eink_scene_bind_display(EinkEpaperDisplay* display) {
    s_display = display;
}

esp_err_t eink_scene_show_frame(const uint8_t* bw_plane, const uint8_t* red_plane) {
    ESP_RETURN_ON_FALSE(s_display != nullptr, ESP_ERR_INVALID_STATE, "eink_scene_legacy", "display not bound");
    return s_display->ShowPackedFrame(bw_plane, red_plane);
}

esp_err_t eink_scene_show_calendar(time_t timestamp) {
    ESP_RETURN_ON_FALSE(s_display != nullptr, ESP_ERR_INVALID_STATE, "eink_scene_legacy", "display not bound");

    if (timestamp <= 0) {
        timestamp = time(nullptr);
    }

    struct tm now_tm;
    struct tm prev_tm;
    struct tm next_tm;
    time_t prev_time = timestamp - (24 * 60 * 60);
    time_t next_time = timestamp + (24 * 60 * 60);
    localtime_r(&timestamp, &now_tm);
    localtime_r(&prev_time, &prev_tm);
    localtime_r(&next_time, &next_tm);

    char header[32];
    char lunar[16];
    std::snprintf(header, sizeof(header), "%04d-%02d", now_tm.tm_year + 1900, now_tm.tm_mon + 1);
    pseudo_lunar_text(&now_tm, lunar, sizeof(lunar));

    s_display->BeginCanvas();
    draw_text_center(52, 6, header, 2);
    draw_text_center(52, 40, "CALENDAR", 1);
    draw_text_center(52, 68, s_week_name[now_tm.tm_wday], 2);

    char day_buf[8];
    std::snprintf(day_buf, sizeof(day_buf), "%02d", now_tm.tm_mday);
    draw_text_center(52, 98, day_buf, 5);
    draw_text_center(52, 160, lunar, 1);

    char side_buf[24];
    std::snprintf(side_buf, sizeof(side_buf), "%02d/%02d", prev_tm.tm_mon + 1, prev_tm.tm_mday);
    draw_text_center(20, 190, side_buf, 1);
    std::snprintf(side_buf, sizeof(side_buf), "%02d/%02d", next_tm.tm_mon + 1, next_tm.tm_mday);
    draw_text_center(84, 190, side_buf, 1);

    return s_display->PresentCanvas();
}

esp_err_t eink_scene_show_memos(const eink_memo_item_t* items, size_t item_count) {
    ESP_RETURN_ON_FALSE(s_display != nullptr, ESP_ERR_INVALID_STATE, "eink_scene_legacy", "display not bound");
    ESP_RETURN_ON_FALSE(items != nullptr, ESP_ERR_INVALID_ARG, "eink_scene_legacy", "memo items null");

    const size_t count = std::min(item_count, static_cast<size_t>(EINK_MEMO_MAX_ITEMS));
    ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_INVALID_ARG, "eink_scene_legacy", "memo items empty");

    s_display->BeginCanvas();
    draw_text_center(52, 6, "MEMO", 2);
    for (size_t i = 0; i < count; ++i) {
        int top = 40 + static_cast<int>(i) * 52;
        fill_rect(4, top, 12, 12, true);
        draw_text_center(10, top + 18, items[i].checked ? "Y" : "N", 1);
        draw_text_center(60, top + 8, items[i].text[0] != '\0' ? items[i].text : "-", 1);
    }
    return s_display->PresentCanvas();
}

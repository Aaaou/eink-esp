#pragma once

#include "esp_err.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define EINK_MEMO_MAX_ITEMS 3
#define EINK_MEMO_TEXT_MAX 32

typedef struct {
    char text[EINK_MEMO_TEXT_MAX];
    bool checked;
} eink_memo_item_t;

esp_err_t eink_scene_show_frame(const uint8_t *bw_plane, const uint8_t *red_plane);
esp_err_t eink_scene_show_calendar(time_t timestamp);
esp_err_t eink_scene_show_memos(const eink_memo_item_t *items, size_t item_count);

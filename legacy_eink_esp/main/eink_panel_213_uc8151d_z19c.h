#pragma once

#include "eink_panel.h"
#include "esp_err.h"

#include <stdint.h>

const eink_panel_descriptor_t *eink_panel_213_uc8151d_z19c_get_descriptor(void);

esp_err_t eink_panel_213_uc8151d_z19c_init(void);
esp_err_t eink_panel_213_uc8151d_z19c_clear_white_gx(void);
esp_err_t eink_panel_213_uc8151d_z19c_display_frame_gx(
    const uint8_t *prev_plane,
    const uint8_t *new_plane,
    uint16_t page_height);
esp_err_t eink_panel_213_uc8151d_z19c_sleep(void);

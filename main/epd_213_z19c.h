#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define EPD_213_Z19C_WIDTH   104
#define EPD_213_Z19C_HEIGHT  212
#define EPD_213_Z19C_BUF_LEN ((EPD_213_Z19C_WIDTH * EPD_213_Z19C_HEIGHT) / 8)

esp_err_t epd_213_z19c_init(void);
esp_err_t epd_213_z19c_clear_white(void);
esp_err_t epd_213_z19c_clear_white_gx(void);
esp_err_t epd_213_z19c_display_frame(const uint8_t *prev_plane, const uint8_t *new_plane);
esp_err_t epd_213_z19c_display_frame_gx(const uint8_t *prev_plane, const uint8_t *new_plane, uint16_t page_height);
esp_err_t epd_213_z19c_sleep(void);

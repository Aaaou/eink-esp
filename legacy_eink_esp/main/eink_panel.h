#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>

#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
#define EINK_PANEL_WIDTH 104U
#define EINK_PANEL_HEIGHT 212U
#define EINK_PANEL_MODEL_ID "213_uc8151d_z19c"
#define EINK_PANEL_CONTROLLER "UC8151D"
#define EINK_PANEL_REFERENCE_DRIVER "GxEPD2_213_Z19c"
#else
#error "Unsupported e-ink panel model selection"
#endif

#define EINK_PANEL_BUF_LEN ((EINK_PANEL_WIDTH * EINK_PANEL_HEIGHT) / 8U)

typedef struct {
    const char *model_id;
    const char *controller;
    const char *reference_driver;
    uint16_t width;
    uint16_t height;
    uint16_t power_on_time_ms;
    uint16_t power_off_time_ms;
    uint16_t clear_refresh_time_ms;
    uint16_t full_refresh_time_ms;
} eink_panel_descriptor_t;

const eink_panel_descriptor_t *eink_panel_get_descriptor(void);

esp_err_t eink_panel_init(void);
esp_err_t eink_panel_clear_white_gx(void);
esp_err_t eink_panel_display_frame_gx(const uint8_t *prev_plane, const uint8_t *new_plane, uint16_t page_height);
esp_err_t eink_panel_sleep(void);

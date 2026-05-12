#include "eink_panel.h"

#include "eink_panel_213_uc8151d_z19c.h"

const eink_panel_descriptor_t *eink_panel_get_descriptor(void)
{
#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
    return eink_panel_213_uc8151d_z19c_get_descriptor();
#endif
}

esp_err_t eink_panel_init(void)
{
#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
    return eink_panel_213_uc8151d_z19c_init();
#endif
}

esp_err_t eink_panel_clear_white_gx(void)
{
#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
    return eink_panel_213_uc8151d_z19c_clear_white_gx();
#endif
}

esp_err_t eink_panel_display_frame_gx(const uint8_t *prev_plane, const uint8_t *new_plane, uint16_t page_height)
{
#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
    return eink_panel_213_uc8151d_z19c_display_frame_gx(prev_plane, new_plane, page_height);
#endif
}

esp_err_t eink_panel_sleep(void)
{
#if CONFIG_INK_PANEL_MODEL_213_UC8151D_Z19C
    return eink_panel_213_uc8151d_z19c_sleep();
#endif
}

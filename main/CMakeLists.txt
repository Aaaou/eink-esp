idf_component_register(
    SRCS
        "app_main.c"
        "board_bringup.c"
        "board_i2c.c"
        "board_spi.c"
        "ble_eink_service.c"
        "eink_scene.c"
        "eink_panel.c"
        "eink_panel_213_uc8151d_z19c.c"
        "eink_bringup.c"
        "pcf8574.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_timer
        nvs_flash
        bt
)

if(CONFIG_INK_SCENE_CJK_FONT_PUHUI_14)
    target_sources(${COMPONENT_LIB} PRIVATE "font_puhui_14_1.c")
endif()

#pragma once

#include "project_defaults.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include <stdbool.h>
#include <stdint.h>

#define BOARD_I2C_PORT                I2C_NUM_0
#define BOARD_I2C_SCL_GPIO            GPIO_NUM_20
#define BOARD_I2C_SDA_GPIO            GPIO_NUM_21
#define BOARD_I2C_FREQ_HZ             400000

#define BOARD_I2S_BCLK_GPIO           GPIO_NUM_3
#define BOARD_I2S_LRCK_GPIO           GPIO_NUM_1
#define BOARD_I2S_MCLK_GPIO           GPIO_NUM_5
// I2S names below are from the ESP32-C3 controller perspective.
// ESP DOUT GPIO0 -> ES8311 DSDIN/DIN, used for playback into the codec.
// ESP DIN  GPIO2 <- ES8311 ASDOUT/DOUT, used for capture from the codec.
#define BOARD_I2S_DOUT_GPIO           GPIO_NUM_0
#define BOARD_I2S_DIN_GPIO            GPIO_NUM_2
#define BOARD_BOOT_BUTTON_GPIO        ((gpio_num_t)CONFIG_AUDIO_RECORD_BUTTON_GPIO)

#define BOARD_EINK_SPI_HOST           SPI2_HOST
#define BOARD_EINK_SCLK_GPIO          GPIO_NUM_7
#define BOARD_EINK_MOSI_GPIO          GPIO_NUM_6
#define BOARD_EINK_CS_GPIO            GPIO_NUM_8
#define BOARD_EINK_DC_GPIO            GPIO_NUM_10
#define BOARD_EINK_BUSY_GPIO          GPIO_NUM_NC
#define BOARD_EINK_SPI_CLOCK_HZ       (4 * 1000 * 1000)

#define BOARD_IOX_ADDR                0x20
#define BOARD_IOX_BIT_EINK_RES        1
#define BOARD_IOX_BIT_EINK_CTRL_4150B 2

#define BOARD_ES8311_ADDR_CANDIDATE0  0x18
#define BOARD_ES8311_ADDR_CANDIDATE1  0x19

#define BOARD_EINK_WIDTH              104
#define BOARD_EINK_HEIGHT             212

#define BOARD_EINK_RESET_HOLD_MS      20
#define BOARD_EINK_RESET_RELEASE_MS   20
#define BOARD_EINK_POWER_SETTLE_MS    30
#define BOARD_EINK_INIT_GUARD_MS      120

typedef struct {
    uint8_t es8311_addr;
    bool es8311_found;
} board_probe_result_t;

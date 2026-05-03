#include "board_spi.h"

#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_spi";
static bool s_spi_ready;

esp_err_t board_eink_spi_init(spi_device_handle_t *out_dev)
{
    ESP_RETURN_ON_FALSE(out_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "out_dev is null");

    if (!s_spi_ready) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num = BOARD_EINK_MOSI_GPIO,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = BOARD_EINK_SCLK_GPIO,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = (BOARD_EINK_WIDTH * BOARD_EINK_HEIGHT / 8) + 64,
        };

        ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_EINK_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                            TAG, "spi_bus_initialize failed");
        s_spi_ready = true;
        ESP_LOGI(TAG, "SPI bus ready: host=%d sclk=%d mosi=%d",
                 BOARD_EINK_SPI_HOST, BOARD_EINK_SCLK_GPIO, BOARD_EINK_MOSI_GPIO);
    }

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BOARD_EINK_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BOARD_EINK_CS_GPIO,
        .queue_size = 4,
    };

    ESP_RETURN_ON_ERROR(spi_bus_add_device(BOARD_EINK_SPI_HOST, &dev_cfg, out_dev),
                        TAG, "spi_bus_add_device failed");

    ESP_LOGI(TAG, "E-ink SPI device ready: cs=%d clock=%d", BOARD_EINK_CS_GPIO, BOARD_EINK_SPI_CLOCK_HZ);
    return ESP_OK;
}

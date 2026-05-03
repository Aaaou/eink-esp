#include "audio_bringup.h"

#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio_bringup";

static esp_err_t audio_init_i2s(void)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;

    const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_LRCK_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din = BOARD_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "tx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "rx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "enable rx failed");

    ESP_LOGI(TAG, "I2S ready: sample_rate=16000 bclk=%d lrck=%d mclk=%d dout=%d din=%d",
             BOARD_I2S_BCLK_GPIO, BOARD_I2S_LRCK_GPIO, BOARD_I2S_MCLK_GPIO,
             BOARD_I2S_DOUT_GPIO, BOARD_I2S_DIN_GPIO);
    ESP_LOGI(TAG, "Speaker path is expected on ES8311 OUTP/OUTN; mic path is expected on MIC1P/MIC1N");
    return ESP_OK;
}

static esp_err_t audio_probe_codec(board_probe_result_t *probe)
{
    probe->es8311_found = false;
    probe->es8311_addr = 0;

    const uint8_t candidates[] = {
        BOARD_ES8311_ADDR_CANDIDATE0,
        BOARD_ES8311_ADDR_CANDIDATE1,
    };

    for (size_t i = 0; i < sizeof(candidates); ++i) {
        uint8_t addr = candidates[i];
        if (board_i2c_probe(addr)) {
            probe->es8311_found = true;
            probe->es8311_addr = addr;
            ESP_LOGI(TAG, "ES8311 candidate responded at 0x%02X", addr);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "ES8311 did not respond on 0x%02X or 0x%02X; schematic did not label the codec address",
             BOARD_ES8311_ADDR_CANDIDATE0, BOARD_ES8311_ADDR_CANDIDATE1);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_bringup_run(board_probe_result_t *probe)
{
    ESP_RETURN_ON_FALSE(probe != NULL, ESP_ERR_INVALID_ARG, TAG, "probe is null");

    ESP_LOGI(TAG, "Starting audio bring-up");
    esp_err_t probe_err = audio_probe_codec(probe);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing with I2S initialization for pin-level validation");
    }

    ESP_RETURN_ON_ERROR(audio_init_i2s(), TAG, "audio I2S init failed");

    if (probe->es8311_found) {
        ESP_LOGI(TAG, "Audio verification hint: validate MCLK/SCLK/LRCK/DOUT/DIN on codec address 0x%02X",
                 probe->es8311_addr);
    }

    return ESP_OK;
}

#include "audio_bringup.h"

#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_check.h"
#include "esp_log.h"
#include "project_defaults.h"
#include "sdkconfig.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio_bringup";
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static es8311_handle_t s_codec;
static bool s_audio_ready;

#define AUDIO_SAMPLE_RATE_HZ CONFIG_AUDIO_RECORD_SAMPLE_RATE
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_BYTES 4
#define AUDIO_INPUT_BLOCK_BYTES 1024
#define AUDIO_I2S_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define AUDIO_I2S_BCLK_DIV 8

typedef struct {
    char riff[4];
    uint32_t file_size_minus_8;
    char wave[4];
    char fmt_[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

typedef struct {
    int32_t min;
    int32_t max;
    uint32_t nonzero;
} sample_diag_t;

static void wav_header_fill(wav_header_t *header, uint32_t data_size)
{
    memcpy(header->riff, "RIFF", 4);
    header->file_size_minus_8 = (uint32_t)(sizeof(wav_header_t) - 8U + data_size);
    memcpy(header->wave, "WAVE", 4);
    memcpy(header->fmt_, "fmt ", 4);
    header->fmt_size = 16;
    header->audio_format = 1;
    header->num_channels = AUDIO_CHANNELS;
    header->sample_rate = AUDIO_SAMPLE_RATE_HZ;
    header->byte_rate = AUDIO_SAMPLE_RATE_HZ * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8U);
    header->block_align = AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8U);
    header->bits_per_sample = AUDIO_BITS_PER_SAMPLE;
    memcpy(header->data, "data", 4);
    header->data_size = data_size;
}

static void sample_diag_update(sample_diag_t *diag, int32_t value)
{
    if (value < diag->min) {
        diag->min = value;
    }
    if (value > diag->max) {
        diag->max = value;
    }
    if (value != 0) {
        ++diag->nonzero;
    }
}

static int32_t sign_extend_24(uint32_t value)
{
    if ((value & 0x00800000U) != 0) {
        value |= 0xFF000000U;
    }
    return (int32_t)value;
}

static void audio_log_raw_diag_once(const uint8_t *buffer, size_t bytes_read)
{
#if CONFIG_AUDIO_RECORD_I2S_RAW_DIAG
    sample_diag_t le16 = { .min = INT32_MAX, .max = INT32_MIN };
    sample_diag_t le16_shift8 = { .min = INT32_MAX, .max = INT32_MIN };
    sample_diag_t be16 = { .min = INT32_MAX, .max = INT32_MIN };
    sample_diag_t le24_hi = { .min = INT32_MAX, .max = INT32_MIN };
    const size_t dump_bytes = (bytes_read < 32) ? bytes_read : 32;
    char dump[3 * 32 + 1] = { 0 };
    size_t pos = 0;

    for (size_t i = 0; i < dump_bytes; ++i) {
        pos += (size_t)snprintf(&dump[pos], sizeof(dump) - pos, "%02X%s", buffer[i], (i + 1 == dump_bytes) ? "" : " ");
    }
    ESP_LOGI(TAG, "[DIAG] first %u raw I2S bytes: %s", (unsigned)dump_bytes, dump);

    for (size_t i = 0; (i + 3) < bytes_read; i += 4) {
        const int16_t l_le16 = (int16_t)((uint16_t)buffer[i] | ((uint16_t)buffer[i + 1] << 8));
        const int16_t r_le16 = (int16_t)((uint16_t)buffer[i + 2] | ((uint16_t)buffer[i + 3] << 8));
        const int16_t l_be16 = (int16_t)(((uint16_t)buffer[i] << 8) | (uint16_t)buffer[i + 1]);
        const int16_t r_be16 = (int16_t)(((uint16_t)buffer[i + 2] << 8) | (uint16_t)buffer[i + 3]);
        const int32_t l_24 = sign_extend_24(((uint32_t)buffer[i + 1]) | ((uint32_t)buffer[i + 2] << 8) |
                                            ((uint32_t)buffer[i + 3] << 16));

        sample_diag_update(&le16, l_le16);
        sample_diag_update(&le16, r_le16);
        sample_diag_update(&le16_shift8, (int32_t)l_le16 >> 8);
        sample_diag_update(&le16_shift8, (int32_t)r_le16 >> 8);
        sample_diag_update(&be16, l_be16);
        sample_diag_update(&be16, r_be16);
        sample_diag_update(&le24_hi, l_24 >> 8);
    }

    ESP_LOGI(TAG, "[DIAG] le16 min=%ld max=%ld nonzero=%lu", (long)le16.min, (long)le16.max, (unsigned long)le16.nonzero);
    ESP_LOGI(TAG, "[DIAG] le16>>8 min=%ld max=%ld nonzero=%lu",
        (long)le16_shift8.min,
        (long)le16_shift8.max,
        (unsigned long)le16_shift8.nonzero);
    ESP_LOGI(TAG, "[DIAG] be16 min=%ld max=%ld nonzero=%lu", (long)be16.min, (long)be16.max, (unsigned long)be16.nonzero);
    ESP_LOGI(TAG, "[DIAG] 24bit-ish min=%ld max=%ld nonzero=%lu",
        (long)le24_hi.min,
        (long)le24_hi.max,
        (unsigned long)le24_hi.nonzero);
#else
    (void)buffer;
    (void)bytes_read;
#endif
}

static esp_err_t audio_init_i2s(void)
{
    const i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 256,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "i2s_new_channel failed");

    const i2s_std_slot_config_t slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_mask = I2S_STD_SLOT_BOTH,
        .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = true,
        .big_endian = false,
        .bit_order_lsb = false,
    };

    const i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = AUDIO_SAMPLE_RATE_HZ,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0,
        .mclk_multiple = AUDIO_I2S_MCLK_MULTIPLE,
        .bclk_div = AUDIO_I2S_BCLK_DIV,
    };

    const i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
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

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "tx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "rx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "enable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "enable rx failed");

    ESP_LOGI(TAG, "[OK] I2S ready: sample_rate=%d BCLK=%d LRCK=%d MCLK=%d DOUT=%d DIN=%d",
        AUDIO_SAMPLE_RATE_HZ,
        BOARD_I2S_BCLK_GPIO,
        BOARD_I2S_LRCK_GPIO,
        BOARD_I2S_MCLK_GPIO,
        BOARD_I2S_DOUT_GPIO,
        BOARD_I2S_DIN_GPIO);
    return ESP_OK;
}

static esp_err_t audio_init_codec(uint8_t codec_addr)
{
    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_SAMPLE_RATE_HZ * 256,
        .sample_frequency = AUDIO_SAMPLE_RATE_HZ,
    };

    s_codec = es8311_create(BOARD_I2C_PORT, codec_addr);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "es8311_create failed");
    ESP_RETURN_ON_ERROR(
        es8311_init(s_codec, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
        TAG,
        "es8311_init failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_codec, false), TAG, "es8311 microphone config failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_30DB), TAG, "es8311 mic gain failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_fade(s_codec, ES8311_FADE_OFF), TAG, "es8311 microphone fade failed");
    ESP_RETURN_ON_ERROR(
        es8311_sample_frequency_config(s_codec, AUDIO_SAMPLE_RATE_HZ * 256, AUDIO_SAMPLE_RATE_HZ),
        TAG,
        "es8311 sample frequency sync failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "[OK] ES8311 capture path configured");
    es8311_register_dump(s_codec);
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
        const uint8_t addr = candidates[i];
        if (board_i2c_probe(addr)) {
            probe->es8311_found = true;
            probe->es8311_addr = addr;
            ESP_LOGI(TAG, "[OK] ES8311 online: 0x%02X", addr);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "[!] ES8311 not responding on 0x%02X / 0x%02X",
        BOARD_ES8311_ADDR_CANDIDATE0,
        BOARD_ES8311_ADDR_CANDIDATE1);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_bringup_run(board_probe_result_t *probe)
{
    ESP_RETURN_ON_FALSE(probe != NULL, ESP_ERR_INVALID_ARG, TAG, "probe is null");

    esp_err_t probe_err = audio_probe_codec(probe);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "[!] Continuing with I2S-only audio bring-up");
    }

    ESP_RETURN_ON_ERROR(audio_init_i2s(), TAG, "audio I2S init failed");
    if (probe->es8311_found) {
        ESP_RETURN_ON_ERROR(audio_init_codec(probe->es8311_addr), TAG, "audio codec init failed");
    }
    s_audio_ready = true;
    return ESP_OK;
}

bool audio_capture_is_ready(void)
{
    return s_audio_ready && (s_i2s_rx != NULL);
}

esp_err_t audio_capture_wav_to_file(const char *path, uint32_t duration_ms, size_t *bytes_written)
{
    ESP_RETURN_ON_FALSE(audio_capture_is_ready(), ESP_ERR_INVALID_STATE, TAG, "audio capture not ready");
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path is null");

    FILE *file = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_FAIL, TAG, "failed to open wav path");

    wav_header_t header;
    wav_header_fill(&header, 0);
    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }

    uint8_t input_buffer[AUDIO_INPUT_BLOCK_BYTES];
    uint8_t mono_buffer[AUDIO_INPUT_BLOCK_BYTES / 2];
    const uint32_t output_bytes_target =
        (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8U) * duration_ms) / 1000U;
    uint32_t total_data_bytes = 0;
    int16_t pcm_min = INT16_MAX;
    int16_t pcm_max = INT16_MIN;
    uint64_t abs_sum = 0;
    uint32_t sample_count = 0;
    uint32_t nonzero_count = 0;
    uint32_t left_nonzero_count = 0;
    uint32_t right_nonzero_count = 0;
    uint32_t raw_nonzero_bytes = 0;
    bool raw_diag_logged = false;

    while (total_data_bytes < output_bytes_target) {
        const uint32_t output_remaining = output_bytes_target - total_data_bytes;
        const size_t mono_want = (output_remaining > sizeof(mono_buffer))
            ? sizeof(mono_buffer)
            : (size_t)output_remaining;
        const size_t input_want = mono_want * 2;
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_i2s_rx, input_buffer, input_want, &bytes_read, pdMS_TO_TICKS(500));
        if ((err != ESP_OK) || (bytes_read == 0)) {
            fclose(file);
            return (err == ESP_OK) ? ESP_FAIL : err;
        }

        if (!raw_diag_logged) {
            audio_log_raw_diag_once(input_buffer, bytes_read);
            raw_diag_logged = true;
        }

        for (size_t i = 0; i < bytes_read; ++i) {
            if (input_buffer[i] != 0) {
                ++raw_nonzero_bytes;
            }
        }

        size_t mono_bytes = 0;
        for (size_t i = 0; (i + AUDIO_FRAME_BYTES - 1) < bytes_read; i += AUDIO_FRAME_BYTES) {
            const int16_t left =
                (int16_t)((uint16_t)input_buffer[i] | ((uint16_t)input_buffer[i + 1] << 8));
            const int16_t right =
                (int16_t)((uint16_t)input_buffer[i + 2] | ((uint16_t)input_buffer[i + 3] << 8));
            const int16_t sample = left;

            mono_buffer[mono_bytes++] = (uint8_t)(sample & 0xFF);
            mono_buffer[mono_bytes++] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);

            if (left != 0) {
                ++left_nonzero_count;
            }
            if (right != 0) {
                ++right_nonzero_count;
            }
            if (sample < pcm_min) {
                pcm_min = sample;
            }
            if (sample > pcm_max) {
                pcm_max = sample;
            }
            if (sample != 0) {
                ++nonzero_count;
            }
            abs_sum += (uint64_t)((sample < 0) ? -(int32_t)sample : sample);
            ++sample_count;
        }

        if ((mono_bytes == 0) || (fwrite(mono_buffer, 1, mono_bytes, file) != mono_bytes)) {
            fclose(file);
            return ESP_FAIL;
        }
        total_data_bytes += (uint32_t)mono_bytes;
    }

    wav_header_fill(&header, total_data_bytes);
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);

    if (bytes_written != NULL) {
        *bytes_written = total_data_bytes + sizeof(header);
    }

    ESP_LOGI(TAG, "[OK] Recorded WAV: %lu bytes payload to %s", (unsigned long)total_data_bytes, path);
    ESP_LOGI(TAG,
        "[OK] PCM stats: samples=%u min=%d max=%d avg_abs=%u nonzero=%u/%u left_nonzero=%u right_nonzero=%u raw_nonzero_bytes=%u",
        sample_count,
        (sample_count > 0) ? pcm_min : 0,
        (sample_count > 0) ? pcm_max : 0,
        (sample_count > 0) ? (unsigned)(abs_sum / sample_count) : 0U,
        nonzero_count,
        sample_count,
        left_nonzero_count,
        right_nonzero_count,
        raw_nonzero_bytes);
    return ESP_OK;
}

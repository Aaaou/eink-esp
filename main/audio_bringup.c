#include "audio_bringup.h"

#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "project_defaults.h"

#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio_bringup";
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_codec_dev_handle_t s_codec_dev;
static const audio_codec_data_if_t *s_codec_data_if;
static const audio_codec_ctrl_if_t *s_codec_ctrl_if;
static const audio_codec_gpio_if_t *s_codec_gpio_if;
static const audio_codec_if_t *s_codec_if;
static bool s_audio_ready;

#define AUDIO_SAMPLE_RATE_HZ CONFIG_AUDIO_RECORD_SAMPLE_RATE
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1
#define AUDIO_CODEC_CHANNELS AUDIO_CHANNELS
#define AUDIO_FRAME_BYTES 2
#define AUDIO_INPUT_BLOCK_BYTES 4096
#define AUDIO_MONO_BLOCK_BYTES AUDIO_INPUT_BLOCK_BYTES
#define AUDIO_I2S_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

typedef struct {
    int16_t pcm_min;
    int16_t pcm_max;
    int64_t sample_sum;
    uint64_t abs_sum;
    uint32_t sample_count;
    uint32_t nonzero_count;
    uint32_t raw_nonzero_bytes;
    uint32_t clip_min_count;
    uint32_t clip_max_count;
    uint32_t read_calls;
    uint32_t short_reads;
    uint32_t min_read_bytes;
    uint32_t max_read_bytes;
    uint32_t max_read_ms;
    uint64_t total_read_ms;
} audio_capture_stats_t;

typedef struct {
    int32_t dc_x_prev;
    int32_t dc_y_prev;
    int32_t noise_floor;
    int32_t agc_gain_q8;
    int16_t prev_raw;
    bool has_prev_raw;
    uint32_t full_scale_spikes;
    uint32_t slew_limited_spikes;
    uint32_t limiter_hits;
} audio_capture_processor_t;

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
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "i2s_new_channel failed");

    const i2s_std_slot_config_t slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
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

static int16_t audio_clip16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static void audio_capture_processor_init(audio_capture_processor_t *processor)
{
    processor->dc_x_prev = 0;
    processor->dc_y_prev = 0;
    processor->noise_floor = 48;
    processor->agc_gain_q8 = 256;
    processor->prev_raw = 0;
    processor->has_prev_raw = false;
    processor->full_scale_spikes = 0;
    processor->slew_limited_spikes = 0;
    processor->limiter_hits = 0;
}

static void audio_capture_log_processing_config(void)
{
    ESP_LOGI(TAG,
        "[MIC] processing=%d noise_gate=%d agc=%d agc_target=%d agc_max_x100=%d limiter=%d",
        CONFIG_AUDIO_CAPTURE_PROCESSING,
        CONFIG_AUDIO_CAPTURE_NOISE_GATE,
        CONFIG_AUDIO_CAPTURE_AGC,
        CONFIG_AUDIO_CAPTURE_AGC_TARGET_AVG,
        CONFIG_AUDIO_CAPTURE_AGC_MAX_GAIN_X100,
        CONFIG_AUDIO_CAPTURE_SOFT_LIMITER);
}

static int16_t audio_capture_remove_spike(audio_capture_processor_t *processor, int16_t input)
{
    int32_t sample = input;
    if (processor->has_prev_raw) {
        const int32_t prev = processor->prev_raw;
        const int32_t delta = sample - prev;
        const int32_t abs_sample = (sample < 0) ? -sample : sample;
        const int32_t abs_delta = (delta < 0) ? -delta : delta;
        if ((sample >= 32760) || (sample <= -32760)) {
            sample = prev;
            ++processor->full_scale_spikes;
        } else if ((abs_sample > 24000) && (abs_delta > 16000)) {
            sample = prev + ((delta > 0) ? 4000 : -4000);
            ++processor->slew_limited_spikes;
        }
    }
    processor->prev_raw = audio_clip16(sample);
    processor->has_prev_raw = true;
    return processor->prev_raw;
}

static int16_t audio_capture_process_sample(audio_capture_processor_t *processor, int16_t input)
{
#if CONFIG_AUDIO_CAPTURE_PROCESSING
    const int32_t x = audio_capture_remove_spike(processor, input);
    int32_t y = x - processor->dc_x_prev + ((processor->dc_y_prev * 32604) >> 15);
    processor->dc_x_prev = x;
    processor->dc_y_prev = y;
    y = audio_clip16(y);

    int32_t abs_y = (y < 0) ? -y : y;
#if CONFIG_AUDIO_CAPTURE_NOISE_GATE
    if (abs_y < (processor->noise_floor * 3)) {
        processor->noise_floor = ((processor->noise_floor * 127) + abs_y) >> 7;
    }
    const int32_t gate_close = processor->noise_floor * 4 + 24;
    const int32_t gate_open = processor->noise_floor * 10 + 80;
    if (abs_y < gate_close) {
        y = 0;
    } else if (abs_y < gate_open) {
        y = (y * (abs_y - gate_close)) / (gate_open - gate_close);
    }
    abs_y = (y < 0) ? -y : y;
#endif

#if CONFIG_AUDIO_CAPTURE_AGC
    const int32_t target_gain_q8 = (abs_y > 64)
        ? ((CONFIG_AUDIO_CAPTURE_AGC_TARGET_AVG << 8) / abs_y)
        : (CONFIG_AUDIO_CAPTURE_AGC_MAX_GAIN_X100 * 256 / 100);
    const int32_t max_gain_q8 = CONFIG_AUDIO_CAPTURE_AGC_MAX_GAIN_X100 * 256 / 100;
    int32_t wanted_gain_q8 = target_gain_q8;
    if (wanted_gain_q8 < 128) {
        wanted_gain_q8 = 128;
    } else if (wanted_gain_q8 > max_gain_q8) {
        wanted_gain_q8 = max_gain_q8;
    }
    if (wanted_gain_q8 < processor->agc_gain_q8) {
        processor->agc_gain_q8 = ((processor->agc_gain_q8 * 3) + wanted_gain_q8) >> 2;
    } else {
        processor->agc_gain_q8 = ((processor->agc_gain_q8 * 31) + wanted_gain_q8) >> 5;
    }
    y = (y * processor->agc_gain_q8) >> 8;
#endif

#if CONFIG_AUDIO_CAPTURE_SOFT_LIMITER
    const int32_t limit = 18000;
    if (y > limit) {
        y = limit + ((y - limit) >> 5);
        ++processor->limiter_hits;
    } else if (y < -limit) {
        y = -limit + ((y + limit) >> 5);
        ++processor->limiter_hits;
    }
    if (y > 22000) {
        y = 22000;
        ++processor->limiter_hits;
    } else if (y < -22000) {
        y = -22000;
        ++processor->limiter_hits;
    }
#endif
    return audio_clip16(y);
#else
    (void)processor;
    return input;
#endif
}

static esp_err_t audio_codec_prepare_output(void)
{
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "codec device is not ready");
    return ESP_OK;
}

static esp_err_t audio_codec_read(uint8_t *buffer, size_t bytes_want, size_t *bytes_read)
{
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "codec device is not ready");
    ESP_RETURN_ON_FALSE(bytes_want <= INT_MAX, ESP_ERR_INVALID_SIZE, TAG, "codec read request too large");

    const int ret = esp_codec_dev_read(s_codec_dev, buffer, (int)bytes_want);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: 0x%X", ret);
        return (ret < 0) ? ret : ESP_FAIL;
    }
    if (bytes_read != NULL) {
        *bytes_read = bytes_want;
    }
    return ESP_OK;
}

static esp_err_t audio_codec_write(uint8_t *buffer, size_t bytes_want, size_t *bytes_written)
{
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "codec device is not ready");
    ESP_RETURN_ON_FALSE(bytes_want <= INT_MAX, ESP_ERR_INVALID_SIZE, TAG, "codec write request too large");

    const int ret = esp_codec_dev_write(s_codec_dev, buffer, (int)bytes_want);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_write failed: 0x%X", ret);
        return (ret < 0) ? ret : ESP_FAIL;
    }
    if (bytes_written != NULL) {
        *bytes_written = bytes_want;
    }
    return ESP_OK;
}

static esp_err_t audio_init_codec(uint8_t codec_addr)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
        .clk_src = I2S_CLK_SRC_DEFAULT,
    };
    s_codec_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_codec_data_if != NULL, ESP_ERR_NO_MEM, TAG, "esp_codec_dev I2S data interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = (uint8_t)(codec_addr << 1),
        .bus_handle = NULL,
    };
    s_codec_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_codec_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "esp_codec_dev I2C control interface failed");

    s_codec_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_codec_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "esp_codec_dev GPIO interface failed");

    es8311_codec_cfg_t es8311_cfg = { 0 };
    es8311_cfg.ctrl_if = s_codec_ctrl_if;
    es8311_cfg.gpio_if = s_codec_gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = -1;
    es8311_cfg.pa_reverted = false;
    es8311_cfg.master_mode = false;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "esp_codec_dev ES8311 interface failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_codec_data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_NO_MEM, TAG, "esp_codec_dev_new failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel = AUDIO_CODEC_CHANNELS,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple = 0,
    };

    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec_dev, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "esp_codec_dev_open failed");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_in_gain(s_codec_dev, (float)CONFIG_AUDIO_ES8311_INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
        ESP_FAIL,
        TAG,
        "esp_codec_dev_set_in_gain failed");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_vol(s_codec_dev, CONFIG_AUDIO_PLAYBACK_VOLUME) == ESP_CODEC_DEV_OK,
        ESP_FAIL,
        TAG,
        "esp_codec_dev_set_out_vol failed");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_mute(s_codec_dev, false) == ESP_CODEC_DEV_OK,
        ESP_FAIL,
        TAG,
        "esp_codec_dev_set_out_mute failed");

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG,
        "[OK] ES8311 configured moji-style: addr7=0x%02X addr8=0x%02X sample_rate=%d codec_channels=%d input_gain=%ddB volume=%d slot=stereo/both/auto",
        codec_addr,
        (unsigned)(codec_addr << 1),
        AUDIO_SAMPLE_RATE_HZ,
        AUDIO_CODEC_CHANNELS,
        CONFIG_AUDIO_ES8311_INPUT_GAIN_DB,
        CONFIG_AUDIO_PLAYBACK_VOLUME);
    esp_codec_dev_dump_reg(s_codec_dev);
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

static bool wav_header_is_usable(const wav_header_t *header)
{
    return (memcmp(header->riff, "RIFF", 4) == 0) &&
           (memcmp(header->wave, "WAVE", 4) == 0) &&
           (memcmp(header->fmt_, "fmt ", 4) == 0) &&
           (memcmp(header->data, "data", 4) == 0) &&
           (header->audio_format == 1) &&
           (header->num_channels == AUDIO_CHANNELS) &&
           (header->sample_rate == AUDIO_SAMPLE_RATE_HZ) &&
           (header->bits_per_sample == AUDIO_BITS_PER_SAMPLE);
}

static void audio_capture_stats_init(audio_capture_stats_t *stats)
{
    stats->pcm_min = INT16_MAX;
    stats->pcm_max = INT16_MIN;
    stats->sample_sum = 0;
    stats->abs_sum = 0;
    stats->sample_count = 0;
    stats->nonzero_count = 0;
    stats->raw_nonzero_bytes = 0;
    stats->clip_min_count = 0;
    stats->clip_max_count = 0;
    stats->read_calls = 0;
    stats->short_reads = 0;
    stats->min_read_bytes = UINT32_MAX;
    stats->max_read_bytes = 0;
    stats->max_read_ms = 0;
    stats->total_read_ms = 0;
}

static void audio_log_capture_summary(
    const char *target,
    uint32_t duration_ms,
    uint32_t elapsed_ms,
    const audio_capture_stats_t *stats,
    uint64_t total_write_ms,
    uint32_t max_write_ms)
{
    ESP_LOGI(TAG,
        "[OK] Capture timing: target=%lu ms elapsed=%lu ms read_calls=%lu short_reads=%lu read_bytes=%lu..%lu read_ms_total=%llu max=%lu write_ms_total=%llu max=%lu",
        (unsigned long)duration_ms,
        (unsigned long)elapsed_ms,
        (unsigned long)stats->read_calls,
        (unsigned long)stats->short_reads,
        (unsigned long)((stats->min_read_bytes == UINT32_MAX) ? 0 : stats->min_read_bytes),
        (unsigned long)stats->max_read_bytes,
        (unsigned long long)stats->total_read_ms,
        (unsigned long)stats->max_read_ms,
        (unsigned long long)total_write_ms,
        (unsigned long)max_write_ms);
    ESP_LOGI(TAG,
        "[OK] PCM stats (%s): samples=%lu min=%d max=%d mean=%d avg_abs=%u nonzero=%lu/%lu clip_min=%lu clip_max=%lu raw_nonzero_bytes=%lu",
        target,
        (unsigned long)stats->sample_count,
        (stats->sample_count > 0) ? stats->pcm_min : 0,
        (stats->sample_count > 0) ? stats->pcm_max : 0,
        (stats->sample_count > 0) ? (int)(stats->sample_sum / (int64_t)stats->sample_count) : 0,
        (stats->sample_count > 0) ? (unsigned)(stats->abs_sum / stats->sample_count) : 0U,
        (unsigned long)stats->nonzero_count,
        (unsigned long)stats->sample_count,
        (unsigned long)stats->clip_min_count,
        (unsigned long)stats->clip_max_count,
        (unsigned long)stats->raw_nonzero_bytes);
    if ((stats->sample_count > 0) &&
        ((stats->abs_sum / stats->sample_count) < 8U) &&
        (stats->pcm_min >= -2) &&
        (stats->pcm_max <= 2)) {
        ESP_LOGW(TAG, "[!] Capture looks near-silent/idle; this playback may sound empty");
    }
}

static esp_err_t audio_capture_pcm_stream(
    uint32_t duration_ms,
    FILE *file,
    uint8_t *output_buffer,
    uint8_t **output_blocks,
    size_t output_block_count,
    size_t output_block_size,
    size_t output_capacity,
    size_t *bytes_written,
    audio_capture_stats_t *stats,
    uint64_t *total_write_ms,
    uint32_t *max_write_ms)
{
    uint8_t *input_buffer = malloc(AUDIO_INPUT_BLOCK_BYTES);
    uint8_t *mono_buffer = malloc(AUDIO_MONO_BLOCK_BYTES);
    if ((input_buffer == NULL) || (mono_buffer == NULL)) {
        free(input_buffer);
        free(mono_buffer);
        return ESP_ERR_NO_MEM;
    }

    const uint32_t output_bytes_target =
        (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8U) * duration_ms) / 1000U;
    if (output_bytes_target > output_capacity) {
        free(input_buffer);
        free(mono_buffer);
        ESP_LOGE(TAG, "PCM output buffer is too small");
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t total_data_bytes = 0;
    bool raw_diag_logged = false;
    audio_capture_processor_t processor;
    audio_capture_processor_init(&processor);
    audio_capture_log_processing_config();

    while (total_data_bytes < output_bytes_target) {
        const uint32_t output_remaining = output_bytes_target - total_data_bytes;
        const size_t mono_want = (output_remaining > AUDIO_MONO_BLOCK_BYTES)
            ? AUDIO_MONO_BLOCK_BYTES
            : (size_t)output_remaining;
        const size_t input_want = mono_want;
        size_t bytes_read = 0;
        const uint32_t read_start_ms = esp_log_timestamp();
        esp_err_t err = audio_codec_read(input_buffer, input_want, &bytes_read);
        const uint32_t read_ms = esp_log_timestamp() - read_start_ms;
        ++stats->read_calls;
        stats->total_read_ms += read_ms;
        if (read_ms > stats->max_read_ms) {
            stats->max_read_ms = read_ms;
        }
        if ((err != ESP_OK) || (bytes_read == 0)) {
            free(input_buffer);
            free(mono_buffer);
            return (err == ESP_OK) ? ESP_FAIL : err;
        }
        if (bytes_read < input_want) {
            ++stats->short_reads;
        }
        if (bytes_read < stats->min_read_bytes) {
            stats->min_read_bytes = bytes_read;
        }
        if (bytes_read > stats->max_read_bytes) {
            stats->max_read_bytes = bytes_read;
        }

        if (!raw_diag_logged) {
            audio_log_raw_diag_once(input_buffer, bytes_read);
            raw_diag_logged = true;
        }

        for (size_t i = 0; i < bytes_read; ++i) {
            if (input_buffer[i] != 0) {
                ++stats->raw_nonzero_bytes;
            }
        }

        size_t mono_bytes = 0;
        for (size_t i = 0; (i + 1U) < bytes_read; i += AUDIO_FRAME_BYTES) {
            const int16_t raw_sample =
                (int16_t)((uint16_t)input_buffer[i] | ((uint16_t)input_buffer[i + 1U] << 8));
            const int16_t sample = audio_capture_process_sample(&processor, raw_sample);

            mono_buffer[mono_bytes++] = (uint8_t)(sample & 0xFF);
            mono_buffer[mono_bytes++] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);

            if (sample < stats->pcm_min) {
                stats->pcm_min = sample;
            }
            if (sample > stats->pcm_max) {
                stats->pcm_max = sample;
            }
            if (sample != 0) {
                ++stats->nonzero_count;
            }
            if (sample == INT16_MIN) {
                ++stats->clip_min_count;
            }
            if (sample == INT16_MAX) {
                ++stats->clip_max_count;
            }
            stats->sample_sum += sample;
            stats->abs_sum += (uint64_t)((sample < 0) ? -(int32_t)sample : sample);
            ++stats->sample_count;
        }

        if (mono_bytes == 0) {
            free(input_buffer);
            free(mono_buffer);
            return ESP_FAIL;
        }

        const uint32_t write_start_ms = esp_log_timestamp();
        if (file != NULL) {
            if (fwrite(mono_buffer, 1, mono_bytes, file) != mono_bytes) {
                free(input_buffer);
                free(mono_buffer);
                return ESP_FAIL;
            }
        } else if (output_buffer != NULL) {
            memcpy(output_buffer + total_data_bytes, mono_buffer, mono_bytes);
        } else {
            size_t copied = 0;
            while (copied < mono_bytes) {
                const size_t absolute_offset = (size_t)total_data_bytes + copied;
                const size_t block_index = absolute_offset / output_block_size;
                const size_t block_offset = absolute_offset % output_block_size;
                const size_t block_remaining = output_block_size - block_offset;
                const size_t copy_now = ((mono_bytes - copied) > block_remaining) ? block_remaining : (mono_bytes - copied);
                if ((block_index >= output_block_count) || (output_blocks[block_index] == NULL)) {
                    free(input_buffer);
                    free(mono_buffer);
                    ESP_LOGE(TAG, "PCM output block is missing");
                    return ESP_ERR_INVALID_SIZE;
                }
                memcpy(output_blocks[block_index] + block_offset, mono_buffer + copied, copy_now);
                copied += copy_now;
            }
        }
        const uint32_t write_ms = esp_log_timestamp() - write_start_ms;
        *total_write_ms += write_ms;
        if (write_ms > *max_write_ms) {
            *max_write_ms = write_ms;
        }
        total_data_bytes += (uint32_t)mono_bytes;
    }

    free(input_buffer);
    free(mono_buffer);
#if CONFIG_AUDIO_CAPTURE_PROCESSING
    ESP_LOGI(TAG,
        "[MIC] process stats: full_scale_spikes=%lu slew_limited_spikes=%lu limiter_hits=%lu final_gain_q8=%ld",
        (unsigned long)processor.full_scale_spikes,
        (unsigned long)processor.slew_limited_spikes,
        (unsigned long)processor.limiter_hits,
        (long)processor.agc_gain_q8);
#endif
    if (bytes_written != NULL) {
        *bytes_written = total_data_bytes;
    }
    return ESP_OK;
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

    const uint32_t output_bytes_target =
        (AUDIO_SAMPLE_RATE_HZ * (AUDIO_BITS_PER_SAMPLE / 8U) * duration_ms) / 1000U;
    size_t total_data_bytes = 0;
    audio_capture_stats_t stats;
    audio_capture_stats_init(&stats);
    uint32_t max_write_ms = 0;
    uint64_t total_write_ms = 0;
    const uint32_t capture_start_ms = esp_log_timestamp();

    esp_err_t err = audio_capture_pcm_stream(
        duration_ms,
        file,
        NULL,
        NULL,
        0,
        0,
        output_bytes_target,
        &total_data_bytes,
        &stats,
        &total_write_ms,
        &max_write_ms);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    wav_header_fill(&header, (uint32_t)total_data_bytes);
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

    const uint32_t elapsed_ms = esp_log_timestamp() - capture_start_ms;
    ESP_LOGI(TAG, "[OK] Recorded WAV: %lu bytes payload to %s", (unsigned long)total_data_bytes, path);
    audio_log_capture_summary("file", duration_ms, elapsed_ms, &stats, total_write_ms, max_write_ms);
    return ESP_OK;
}

esp_err_t audio_capture_pcm_to_buffer(uint8_t *pcm_buffer, size_t buffer_size, uint32_t duration_ms, size_t *bytes_written)
{
    ESP_RETURN_ON_FALSE(audio_capture_is_ready(), ESP_ERR_INVALID_STATE, TAG, "audio capture not ready");
    ESP_RETURN_ON_FALSE(pcm_buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "PCM buffer is null");

    audio_capture_stats_t stats;
    audio_capture_stats_init(&stats);
    uint32_t max_write_ms = 0;
    uint64_t total_write_ms = 0;
    size_t total_data_bytes = 0;
    const uint32_t capture_start_ms = esp_log_timestamp();

    esp_err_t err = audio_capture_pcm_stream(
        duration_ms,
        NULL,
        pcm_buffer,
        NULL,
        0,
        0,
        buffer_size,
        &total_data_bytes,
        &stats,
        &total_write_ms,
        &max_write_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (bytes_written != NULL) {
        *bytes_written = total_data_bytes;
    }

    const uint32_t elapsed_ms = esp_log_timestamp() - capture_start_ms;
    ESP_LOGI(TAG, "[OK] Recorded PCM: %lu bytes payload to RAM", (unsigned long)total_data_bytes);
    audio_log_capture_summary("ram", duration_ms, elapsed_ms, &stats, total_write_ms, max_write_ms);
    return ESP_OK;
}

esp_err_t audio_capture_pcm_to_blocks(uint8_t **blocks, size_t block_count, size_t block_size, uint32_t duration_ms, size_t *bytes_written)
{
    ESP_RETURN_ON_FALSE(audio_capture_is_ready(), ESP_ERR_INVALID_STATE, TAG, "audio capture not ready");
    ESP_RETURN_ON_FALSE((blocks != NULL) && (block_count > 0) && (block_size > 0), ESP_ERR_INVALID_ARG, TAG, "bad PCM blocks");

    audio_capture_stats_t stats;
    audio_capture_stats_init(&stats);
    uint32_t max_write_ms = 0;
    uint64_t total_write_ms = 0;
    size_t total_data_bytes = 0;
    const uint32_t capture_start_ms = esp_log_timestamp();

    esp_err_t err = audio_capture_pcm_stream(
        duration_ms,
        NULL,
        NULL,
        blocks,
        block_count,
        block_size,
        block_count * block_size,
        &total_data_bytes,
        &stats,
        &total_write_ms,
        &max_write_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (bytes_written != NULL) {
        *bytes_written = total_data_bytes;
    }

    const uint32_t elapsed_ms = esp_log_timestamp() - capture_start_ms;
    ESP_LOGI(TAG, "[OK] Recorded PCM: %lu bytes payload to RAM blocks", (unsigned long)total_data_bytes);
    audio_log_capture_summary("ram-blocks", duration_ms, elapsed_ms, &stats, total_write_ms, max_write_ms);
    return ESP_OK;
}

esp_err_t audio_play_wav_file(const char *path)
{
    ESP_RETURN_ON_FALSE(s_audio_ready && (s_i2s_tx != NULL), ESP_ERR_INVALID_STATE, TAG, "audio playback not ready");
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path is null");

    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_ERR_NOT_FOUND, TAG, "failed to open wav for playback");

    wav_header_t header;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }
    if (!wav_header_is_usable(&header)) {
        fclose(file);
        ESP_LOGE(TAG, "[PLAY] Unsupported WAV header: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(audio_codec_prepare_output(), TAG, "ES8311 output prepare failed");

    uint8_t *mono_buffer = malloc(AUDIO_MONO_BLOCK_BYTES);
    if (mono_buffer == NULL) {
        free(mono_buffer);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    uint32_t remaining = header.data_size;
    uint32_t played_bytes = 0;
    const uint32_t start_ms = esp_log_timestamp();

    ESP_LOGI(TAG, "[PLAY] start path=%s payload=%lu", path, (unsigned long)header.data_size);
    while (remaining > 0) {
        const size_t want = (remaining > AUDIO_MONO_BLOCK_BYTES) ? AUDIO_MONO_BLOCK_BYTES : (size_t)remaining;
        const size_t got = fread(mono_buffer, 1, want, file);
        if (got == 0) {
            free(mono_buffer);
            fclose(file);
            return ESP_FAIL;
        }

        size_t bytes_written = 0;
        esp_err_t err = audio_codec_write(mono_buffer, got, &bytes_written);
        if (err != ESP_OK) {
            free(mono_buffer);
            fclose(file);
            ESP_LOGE(TAG, "[PLAY] codec write failed: %s", esp_err_to_name(err));
            return err;
        }
        if (bytes_written != got) {
            ESP_LOGW(TAG, "[PLAY] short write: %u/%u", (unsigned)bytes_written, (unsigned)got);
        }

        played_bytes += (uint32_t)got;
        remaining -= (uint32_t)got;
    }

    fclose(file);
    free(mono_buffer);
    ESP_LOGI(TAG, "[PLAY] done elapsed=%lu ms payload=%lu", (unsigned long)(esp_log_timestamp() - start_ms), (unsigned long)played_bytes);
    return ESP_OK;
}

esp_err_t audio_play_pcm_buffer(const uint8_t *pcm_buffer, size_t pcm_bytes)
{
    ESP_RETURN_ON_FALSE(s_audio_ready && (s_i2s_tx != NULL), ESP_ERR_INVALID_STATE, TAG, "audio playback not ready");
    ESP_RETURN_ON_FALSE(pcm_buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "PCM buffer is null");
    ESP_RETURN_ON_FALSE((pcm_bytes > 0) && ((pcm_bytes % 2U) == 0), ESP_ERR_INVALID_ARG, TAG, "bad PCM byte count");
    ESP_RETURN_ON_ERROR(audio_codec_prepare_output(), TAG, "ES8311 output prepare failed");

    size_t offset = 0;
    uint32_t played_bytes = 0;
    const uint32_t start_ms = esp_log_timestamp();

    ESP_LOGI(TAG, "[PLAY] start RAM PCM payload=%u", (unsigned)pcm_bytes);
    while (offset < pcm_bytes) {
        const size_t remaining = pcm_bytes - offset;
        const size_t mono_bytes = (remaining > AUDIO_MONO_BLOCK_BYTES) ? AUDIO_MONO_BLOCK_BYTES : remaining;

        size_t bytes_written = 0;
        esp_err_t err = audio_codec_write((uint8_t *)&pcm_buffer[offset], mono_bytes, &bytes_written);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_written != mono_bytes) {
            ESP_LOGW(TAG, "[PLAY] RAM PCM short write: %u/%u", (unsigned)bytes_written, (unsigned)mono_bytes);
        }

        offset += mono_bytes;
        played_bytes += (uint32_t)mono_bytes;
    }

    ESP_LOGI(TAG, "[PLAY] done RAM PCM elapsed=%lu ms payload=%lu", (unsigned long)(esp_log_timestamp() - start_ms), (unsigned long)played_bytes);
    return ESP_OK;
}

esp_err_t audio_play_pcm_blocks(uint8_t *const *blocks, size_t block_count, size_t block_size, size_t pcm_bytes)
{
    ESP_RETURN_ON_FALSE(s_audio_ready && (s_i2s_tx != NULL), ESP_ERR_INVALID_STATE, TAG, "audio playback not ready");
    ESP_RETURN_ON_FALSE((blocks != NULL) && (block_count > 0) && (block_size > 0), ESP_ERR_INVALID_ARG, TAG, "bad PCM blocks");
    ESP_RETURN_ON_FALSE((pcm_bytes > 0) && ((pcm_bytes % 2U) == 0), ESP_ERR_INVALID_ARG, TAG, "bad PCM byte count");
    ESP_RETURN_ON_ERROR(audio_codec_prepare_output(), TAG, "ES8311 output prepare failed");

    uint8_t *mono_buffer = malloc(AUDIO_MONO_BLOCK_BYTES);
    if (mono_buffer == NULL) {
        free(mono_buffer);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    uint32_t played_bytes = 0;
    const uint32_t start_ms = esp_log_timestamp();
#if CONFIG_AUDIO_LOOPBACK_DC_BLOCK
    int32_t dc_x_prev = 0;
    int32_t dc_y_prev = 0;
#endif

    ESP_LOGI(TAG, "[PLAY] start RAM block PCM payload=%u volume=%d", (unsigned)pcm_bytes, CONFIG_AUDIO_PLAYBACK_VOLUME);
    while (offset < pcm_bytes) {
        const size_t remaining = pcm_bytes - offset;
        const size_t mono_bytes = (remaining > AUDIO_MONO_BLOCK_BYTES) ? AUDIO_MONO_BLOCK_BYTES : remaining;
        size_t copied = 0;

        while (copied < mono_bytes) {
            const size_t absolute_offset = offset + copied;
            const size_t block_index = absolute_offset / block_size;
            const size_t block_offset = absolute_offset % block_size;
            const size_t block_remaining = block_size - block_offset;
            const size_t copy_now = ((mono_bytes - copied) > block_remaining) ? block_remaining : (mono_bytes - copied);
            if ((block_index >= block_count) || (blocks[block_index] == NULL)) {
                free(mono_buffer);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(mono_buffer + copied, blocks[block_index] + block_offset, copy_now);
            copied += copy_now;
        }

        for (size_t i = 0; (i + 1U) < mono_bytes; i += 2U) {
            int16_t sample = (int16_t)((uint16_t)mono_buffer[i] | ((uint16_t)mono_buffer[i + 1U] << 8));
#if CONFIG_AUDIO_LOOPBACK_DC_BLOCK
            const int32_t x = sample;
            int32_t y = x - dc_x_prev + ((dc_y_prev * 32604) >> 15);
            dc_x_prev = x;
            dc_y_prev = y;
            if (y > INT16_MAX) {
                y = INT16_MAX;
            } else if (y < INT16_MIN) {
                y = INT16_MIN;
            }
            sample = (int16_t)y;
#endif
            mono_buffer[i] = (uint8_t)(sample & 0xFF);
            mono_buffer[i + 1U] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);
        }

        size_t bytes_written = 0;
        esp_err_t err = audio_codec_write(mono_buffer, mono_bytes, &bytes_written);
        if (err != ESP_OK) {
            free(mono_buffer);
            return err;
        }
        if (bytes_written != mono_bytes) {
            ESP_LOGW(TAG, "[PLAY] RAM block PCM short write: %u/%u", (unsigned)bytes_written, (unsigned)mono_bytes);
        }

        offset += mono_bytes;
        played_bytes += (uint32_t)mono_bytes;
    }

    free(mono_buffer);
    ESP_LOGI(TAG, "[PLAY] done RAM block PCM elapsed=%lu ms payload=%lu", (unsigned long)(esp_log_timestamp() - start_ms), (unsigned long)played_bytes);
    return ESP_OK;
}

esp_err_t audio_play_test_tone(uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(s_audio_ready && (s_i2s_tx != NULL), ESP_ERR_INVALID_STATE, TAG, "audio playback not ready");
    ESP_RETURN_ON_FALSE(duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "test tone duration is zero");

    ESP_RETURN_ON_ERROR(audio_codec_prepare_output(), TAG, "ES8311 output prepare failed");

    int16_t *mono_buffer = malloc(AUDIO_INPUT_BLOCK_BYTES);
    ESP_RETURN_ON_FALSE(mono_buffer != NULL, ESP_ERR_NO_MEM, TAG, "test tone buffer alloc failed");

    const uint32_t total_frames = (AUDIO_SAMPLE_RATE_HZ * duration_ms) / 1000U;
    uint32_t frames_done = 0;
    uint32_t phase = 0;
    const uint32_t phase_step_a = ((uint32_t)880U << 16) / AUDIO_SAMPLE_RATE_HZ;
    const uint32_t phase_step_b = ((uint32_t)1320U << 16) / AUDIO_SAMPLE_RATE_HZ;
    const uint32_t frames_per_buffer = AUDIO_INPUT_BLOCK_BYTES / sizeof(int16_t);
    const uint32_t split_frame = total_frames / 2U;
    const uint32_t start_ms = esp_log_timestamp();

    ESP_LOGI(TAG, "[PLAY] boot speaker test tone start duration=%lu ms", (unsigned long)duration_ms);
    while (frames_done < total_frames) {
        const uint32_t frames_remaining = total_frames - frames_done;
        const uint32_t frames_this = (frames_remaining > frames_per_buffer) ? frames_per_buffer : frames_remaining;
        const uint32_t phase_step = (frames_done < split_frame) ? phase_step_a : phase_step_b;

        for (uint32_t i = 0; i < frames_this; ++i) {
            phase += phase_step;
            const int16_t sample = (phase & 0x8000U) ? 9000 : -9000;
            mono_buffer[i] = sample;
        }

        size_t bytes_written = 0;
        const size_t bytes_to_write = frames_this * sizeof(int16_t);
        esp_err_t err = audio_codec_write((uint8_t *)mono_buffer, bytes_to_write, &bytes_written);
        if (err != ESP_OK) {
            free(mono_buffer);
            return err;
        }
        if (bytes_written != bytes_to_write) {
            ESP_LOGW(TAG, "[PLAY] test tone short write: %u/%u", (unsigned)bytes_written, (unsigned)bytes_to_write);
        }
        frames_done += frames_this;
    }

    free(mono_buffer);
    ESP_LOGI(TAG, "[PLAY] boot speaker test tone done elapsed=%lu ms", (unsigned long)(esp_log_timestamp() - start_ms));
    return ESP_OK;
}

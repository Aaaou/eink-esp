#pragma once

#include "board_config.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_bringup_run(board_probe_result_t *probe);
bool audio_capture_is_ready(void);
esp_err_t audio_capture_wav_to_file(const char *path, uint32_t duration_ms, size_t *bytes_written);
esp_err_t audio_capture_pcm_to_buffer(uint8_t *pcm_buffer, size_t buffer_size, uint32_t duration_ms, size_t *bytes_written);
esp_err_t audio_capture_pcm_to_blocks(uint8_t **blocks, size_t block_count, size_t block_size, uint32_t duration_ms, size_t *bytes_written);
esp_err_t audio_play_wav_file(const char *path);
esp_err_t audio_play_pcm_buffer(const uint8_t *pcm_buffer, size_t pcm_bytes);
esp_err_t audio_play_pcm_blocks(uint8_t *const *blocks, size_t block_count, size_t block_size, size_t pcm_bytes);
esp_err_t audio_play_test_tone(uint32_t duration_ms);

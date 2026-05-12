# Development Log

## 2026-05-10 - ES8311 audio, flash recording, and AP portal

### Branches

- Main development branch: `development`
- Working integration branch: `codex/save-dev-audio-flash-20260510`
- Audio baseline reference branch used for hardware checks: `codex/recording-diagnostics`

### Current Audio Baseline

- ES8311 is initialized through `esp_codec_dev`.
- I2S pins:
  - BCLK: GPIO3
  - LRCK: GPIO1
  - MCLK: GPIO5
  - DOUT: GPIO0
  - DIN: GPIO2
- Sample rate: `24000 Hz`
- Codec channels: mono capture/playback path using the Moji/Xiaozhi-style codec setup.
- Input gain: `30 dB`
- Volume: `100`
- Boot speaker test tone is kept, but moved after storage, portal, and recording service initialization so codec and I2S are already stable when the tone starts.

### Recording Behavior

- Removed the RAM loopback recording path.
- BOOT short press now records to SPIFFS Flash when no valid recording is available.
- The recording file is always:
  - `/spiffs/record.wav`
- Only the newest recording is retained.
- Old slot-style files are removed before a new recording starts:
  - `/spiffs/record_slot_1.wav`
  - `/spiffs/record_slot_2.wav`
  - `/spiffs/record_slot_3.wav`
  - `/spiffs/rec_000001.wav` through `/spiffs/rec_000005.wav`
- BOOT short press plays the existing recording through ES8311 when a valid recording is present.
- BOOT long press starts the AP portal.

### AP Portal Recording Features

- The web page keeps physical-button recording as the main recording path.
- Web recording was intentionally not reintroduced, because AP latency and request timing can interfere with the desired local BOOT-button workflow.
- The AP page now supports:
  - Refresh recording list
  - Browser playback through `/api/record.wav`
  - Download current recording
  - ES8311 device playback through `POST /api/record/play`
  - Delete current recording through `POST /api/record/delete`
- Delete is blocked while recording or playback is active.

### Wi-Fi / Portal Behavior

- Saved STA autoconnect is disabled while the portal AP is open.
- This avoids the previous AP instability where STA connect/scan activity changed channels or blocked scan requests while a phone was connected to `INK-ESP-AUDIO`.
- The portal remains AP/APSTA-capable for scan and connect flows, but it does not automatically join a saved router in the background while the user is using the portal.

### Important Bug Fixed

Opening the AP page while using the physical BOOT button could previously break recording:

```text
[REC] Removed invalid recording file: /spiffs/record.wav
[LOOP xxxxxx] record failed: ESP_FAIL
```

Root cause:

- `/api/status` or `/api/records` refreshed while recording was in progress.
- The recording file existed but was not yet a complete WAV.
- The status refresh code treated this partial file as invalid and removed it.
- The active recording task then failed because its output file had been deleted.

Fix:

- Recording status refresh no longer validates or deletes WAV files when recording, pending recording, playback, or pending playback is active.
- Invalid WAV cleanup only happens while the audio service is idle.

### Memory Notes

- ESP32-C3FN4 has limited RAM for dynamic allocation.
- A 3 second, 24 kHz, 16-bit, mono PCM buffer is about `144000` bytes before WAV header overhead.
- Keeping the recording in RAM is not a good fit for this device once Wi-Fi, HTTP server, SPIFFS, e-paper, and codec tasks are also active.
- Flash-backed recording is the preferred path for this project.

### Current Validation Status

Confirmed in logs before the final cleanup:

- BOOT short press can record to `/spiffs/record.wav`.
- BOOT short press can play the recording through ES8311.
- Browser playback can request `/api/record.wav`.
- Web ES8311 playback can call `/api/record/play`.
- Web delete can delete `/spiffs/record.wav`.

Needs validation after the final race-condition fix:

- Keep AP page open.
- Press BOOT to record.
- Confirm no log appears with:

```text
[REC] Removed invalid recording file: /spiffs/record.wav
```

- Confirm the recording completes with a valid file size near:

```text
144044
```

### Known Hardware Notes

- The codec register dump shows ES8311 online at `0x18`.
- Previous ADCR/DAC reference debugging indicated hardware assembly issues can directly affect codec behavior.
- If ES8311 playback logs show success but the speaker is silent, check the speaker path, amplifier path, and board-level output wiring before changing audio software.


# ES8311 Audio Bring-up Notes

Date: 2026-05-08

## Current Status

- ES8311 is detected on I2C address `0x18`.
- The board I2S wiring is now documented with both codec-side and ESP-side names in [c3墨水屏接线表格.md](/D:/DEMO/ink_esp/c3墨水屏接线表格.md).
- The firmware pin mapping in [main/board_config.h](/D:/DEMO/ink_esp/main/board_config.h) must use ESP-IDF's I2S-controller perspective:

| Signal | Codec-side name | ESP32-C3 GPIO | ESP-IDF field | Direction |
| :-- | :-- | :-- | :-- | :-- |
| Master clock | `MCLK` | GPIO5 | `mclk` | ESP -> ES8311 |
| Bit clock | `SCLK` / `BCLK` | GPIO3 | `bclk` | ESP -> ES8311 |
| Word select | `LRCK` | GPIO1 | `ws` | ESP -> ES8311 |
| Playback data | ES8311 `DSDIN` / schematic `DIN` | GPIO0 | `dout` | ESP -> ES8311 |
| Capture data | ES8311 `ASDOUT` / schematic `DOUT` | GPIO2 | `din` | ES8311 -> ESP |
| I2C | `SCL` / `SDA` | GPIO20 / GPIO21 | I2C | bidirectional control |

The important naming rule is:

- schematic `DIN` means ES8311 data input, so it is ESP-IDF I2S `dout`
- schematic `DOUT` means ES8311 data output, so it is ESP-IDF I2S `din`

Expected boot log after the corrected mapping:

```text
I2S ready: sample_rate=16000 BCLK=3 LRCK=1 MCLK=5 DOUT=0 DIN=2
```

## What Was Already Changed

The current [audio_bringup.c](/D:/DEMO/ink_esp/main/audio_bringup.c) is not the earliest simple version anymore.
It already includes a partial migration toward a xiaozhi-style setup:

- explicit I2S standard-mode configuration instead of only relying on the helper macro
- stereo slot receive path
- fixed `16-bit` slot width
- `MCLK = sample_rate * 256`
- microphone gain set to `30 dB`
- raw capture diagnostics including:
  - `left_nonzero`
  - `right_nonzero`
  - `raw_nonzero_bytes`
  - one-shot raw I2S byte dump when `CONFIG_AUDIO_RECORD_I2S_RAW_DIAG=y`

So the answer to "has ES8311 already been initialized and recorded in a xiaozhi-like way?" is:

- `partly yes` on the I2S framing side
- `not fully yes` on the codec-stack side

We did **not** migrate to the full xiaozhi audio stack such as `esp_codec_dev` or its higher-level board codec abstraction.
We only moved the current raw ES8311 + I2S setup closer to that style.

## Diagnostic Timeline

Before the DIN/DOUT direction fix, the firmware was effectively listening on the wrong I2S data pin.
The recording path produced a valid WAV container, but the PCM payload was fully zero:

```text
PCM stats: samples=80000 min=0 max=0 avg_abs=0 nonzero=0/80000 left_nonzero=0 right_nonzero=0 raw_nonzero_bytes=0
```

After correcting the ESP-IDF mapping to `DOUT=GPIO0` and `DIN=GPIO2`, the ESP32-C3 can read digital data from the ES8311 output pin.
The current log is no longer all zero:

```text
Recorded WAV: 320000 bytes payload to /spiffs/record.wav
PCM stats: samples=160000 min=-1 max=0 avg_abs=0 nonzero=44428/160000 left_nonzero=44428 right_nonzero=44428 raw_nonzero_bytes=177712
```

The raw I2S diagnostic also shows the captured stream is mostly `0x00` and `0xFF`, which decodes as `0` and `-1` in common 16-bit interpretations:

```text
[DIAG] first 32 raw I2S bytes: 00 00 00 00 00 00 00 00 FF FF FF FF 00 00 00 00 FF FF FF FF 00 00 00 00 FF FF FF FF 00 00 00 00
[DIAG] le16 min=-1 max=0 nonzero=142
[DIAG] le16>>8 min=-1 max=0 nonzero=142
[DIAG] be16 min=-1 max=0 nonzero=142
[DIAG] 24bit-ish min=-1 max=0 nonzero=71
```

## Current Interpretation

The latest evidence can reasonably exclude these as the primary issue:

- WAV file creation
- SPIFFS storage
- HTTP download
- ESP32-C3 reading from a totally idle I2S RX pin
- a simple little-endian vs big-endian mistake
- a simple 16-bit vs 24-bit unpacking mistake
- the earlier ESP-IDF `.din/.dout` GPIO direction error

What remains most suspicious:

1. ES8311 ADC input routing, PGA, ADC mute, or microphone-related register setup is still not exactly right for this board.
2. The analog microphone signal may not be reaching the ES8311 ADC input with usable amplitude.
3. The microphone capsule or its bias/coupling path may have an assembly issue, even though static DC readings look plausible.

Important nuance:

- The current data proves ES8311 is shifting some digital capture data back to the ESP.
- It does **not** prove that the microphone analog path is producing real audio.
- It does **not** prove the microphone is damaged either; it only says the codec output is still near digital silence.

## Microphone Schematic Observation

The uploaded microphone fragment shows:

- `MICP` and `MICN` routed through coupling capacitors into the codec input
- `VREF` and local analog filtering around the microphone front end
- `+3V3` feeding the bias network through ferrite beads and decoupling

At a high level, this is a normal-looking analog microphone front-end pattern.
Nothing immediately looks fundamentally wrong from this fragment alone.

## Can 3.3V On The Coupling Caps Prove Audio Is Fine?

No.

What it proves:

- the bias network is probably alive
- the microphone front end is probably not totally open-circuit

What it does **not** prove:

- the differential audio waveform is reaching the ES8311 ADC correctly
- the ADC / PGA path is enabled correctly
- the codec is producing meaningful non-silent audio samples
- the microphone capsule itself is healthy

## PCB Routing Guidance

For this design class:

- `MICP/MICN` should be kept short and away from noisy switching nodes
- if possible, route microphone inputs as a close pair
- avoid running them parallel to Wi-Fi antenna feed, SPI clock, I2S clock, or DC/DC switching nodes
- analog bias / reference decoupling should stay physically close to the codec

For digital audio lines:

- `MCLK`, `BCLK`, `LRCK`, `DSDIN`, and `ASDOUT` do not require controlled impedance on a small board at these lengths
- they do benefit from:
  - short traces
  - solid reference ground underneath when possible
  - avoiding long parallel runs with the microphone input pair

About ground pour / shielding:

- yes, keeping a continuous ground reference under the digital audio lines is helpful
- yes, the microphone analog area benefits from quiet local ground and physical separation from noisy clocks
- no, these short I2S traces usually do not need special shielding or exotic impedance treatment

In plain terms:

- analog mic traces: treat gently
- I2S traces: keep short and referenced to ground
- the biggest risk is usually analog noise coupling or analog input routing, not transmission-line behavior

## Next Hardware Check

When a scope or logic analyzer is available, observe these while a recording is active and someone is speaking near the microphone:

1. `GPIO5 / MCLK`
2. `GPIO3 / BCLK`
3. `GPIO1 / LRCK`
4. `GPIO2 / ES8311 ASDOUT(DOUT) / ESP DIN`

Interpretation:

- if `MCLK/BCLK/LRCK` are missing, the problem is on the ESP32-C3 I2S clock side
- if clocks exist but `GPIO2 / ASDOUT` is stuck flat, the problem is likely inside ES8311 ADC/output routing
- if `GPIO2 / ASDOUT` toggles but only between near-zero codes while speaking, focus on ADC input routing, PGA, MICBIAS, microphone capsule, or analog front-end assembly
- `GPIO0 / ES8311 DSDIN(DIN) / ESP DOUT` is playback input to the codec and is less useful for debugging microphone capture

## Separate Note About Wi-Fi Brownout

Earlier Wi-Fi brownout behavior was very likely a power-path issue related to the TP4056 `BAT` node being unstable without a battery attached.
After adding a battery, SoftAP startup became stable.

This appears to be independent from the current near-silent audio capture issue.

# ES8311 Audio Bring-up Notes

Date: 2026-05-07

## Current Status

- ES8311 is detected on I2C address `0x18`.
- The board pin mapping in [main/board_config.h](/D:/DEMO/ink_esp/main/board_config.h) matches the wiring sheet in [c3墨水屏接线表格.md](/D:/DEMO/ink_esp/c3墨水屏接线表格.md):
  - `GPIO0 -> DIN`
  - `GPIO1 -> LRCK`
  - `GPIO2 -> DOUT`
  - `GPIO3 -> BCLK`
  - `GPIO5 -> MCLK`
  - `GPIO20/21 -> I2C`
- Audio recording produces a valid WAV container, but the captured PCM stream is fully zero.

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

So the answer to "has ES8311 already been initialized and recorded in a xiaozhi-like way?" is:

- `partly yes` on the I2S framing side
- `not fully yes` on the codec-stack side

We did **not** migrate to the full xiaozhi audio stack such as `esp_codec_dev` or its higher-level board codec abstraction.
We only moved the current raw ES8311 + I2S setup closer to that style.

## Most Important Diagnostic Result

Latest log:

`PCM stats: samples=80000 min=0 max=0 avg_abs=0 nonzero=0/80000 left_nonzero=0 right_nonzero=0 raw_nonzero_bytes=0`

This is the key finding.

Interpretation:

- the WAV file is not the problem
- byte extraction is not the problem
- mono vs stereo unpacking is not the problem
- ESP32-C3 I2S RX currently receives an all-zero stream

That narrows the likely fault boundary to one of these:

1. ES8311 ADC / serial output path is not actually producing digital samples
2. I2S clocks are not really active on hardware even though software configuration is correct
3. analog microphone bias exists, but the codec input routing still does not match the board topology

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
- the codec is really shifting out digital audio on `DIN`

## PCB Routing Guidance

For this design class:

- `MICP/MICN` should be kept short and away from noisy switching nodes
- if possible, route microphone inputs as a close pair
- avoid running them parallel to Wi-Fi antenna feed, SPI clock, I2S clock, or DC/DC switching nodes
- analog bias / reference decoupling should stay physically close to the codec

For digital audio lines:

- `MCLK`, `BCLK`, `LRCK`, and `DIN` do not require controlled impedance on a small board at these lengths
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
- the biggest risk is usually analog noise coupling, not transmission-line behavior

## Next Hardware Check

When a scope or logic analyzer is available, observe:

1. `GPIO5 / MCLK`
2. `GPIO3 / BCLK`
3. `GPIO1 / LRCK`
4. `GPIO0 / DIN`

While a recording is active and someone is speaking near the microphone:

- if `MCLK/BCLK/LRCK` are missing, the problem is on the ESP32-C3 I2S clock side
- if clocks exist but `DIN` stays flat, the problem is likely inside ES8311 ADC/output routing
- if `DIN` toggles but software still sees zeros, the remaining issue is I2S receive interpretation

## Separate Note About Wi-Fi Brownout

Earlier Wi-Fi brownout behavior was very likely a power-path issue related to the TP4056 `BAT` node being unstable without a battery attached.
After adding a battery, SoftAP startup became stable.

This appears to be independent from the all-zero audio capture issue.

# Eink C3 BLE Primitives Draft

## Purpose

This document captures the current `development`-branch web-side data model and image packing rules
so a future BLE mini-program can implement the same feature set without reverse-reading browser code.

## Memo Data Shape

- Up to 3 memo items per refresh.
- Each item contains:
  - `text`: UTF-8 string
  - `checked`: boolean
- Empty text items are ignored.
- The current firmware-side implementation limits each visible text field to short memo text.

## Calendar Input

- Calendar refresh is driven from a Unix timestamp.
- Input payload concept:
  - `timestamp`: seconds since Unix epoch
- The rendered result is a fixed device-side calendar layout, not arbitrary host-drawn pixels.

## Image Array / Frame Packing

- Source image is normalized to the panel resolution.
- Current panel target size:
  - width: `104`
  - height: `212`
- Output is a packed 1bpp-style frame model intended for e-paper usage.
- Existing web pipeline conceptually performs:
  - crop
  - scale
  - grayscale / palette reduction
  - threshold / dithering selection
  - black plane packing
  - optional red plane packing
- Future BLE protocol must document:
  - byte order within each row
  - plane ordering
  - refresh mode flag
  - total payload length

## Planned Follow-up

- Replace this draft with exact byte layout and examples after the board-side e-ink frame interface is finalized.

# Legacy `eink-esp` Reference Source

This directory preserves the original `eink-esp` implementation as a migration reference while the repository root is being reshaped around the XiaoZhi codebase.

## Source Baseline

The files in this directory were extracted from the validated `development`-branch baseline at commit:

- `814f513` - `Merge flash recording portal integration`

## Why this directory exists

The XiaoZhi integration work needs a stable source-of-truth for:

- board wiring
- ES8311 audio behavior
- `PCF8574 -> EINK_RES / 4150B` control chain
- e-ink panel init and refresh sequence
- memo / calendar / image-frame logic
- flash-backed recording and playback behavior

Instead of keeping those details only in git history, this directory preserves the relevant implementation files in a single explicit reference tree.

## Intended Usage

Use this directory as the migration source when porting functionality into the XiaoZhi-oriented architecture in the repository root.

Typical examples:

- migrate board-level wiring into `main/boards/eink-c3-board/config.h`
- migrate `PCF8574` control semantics into the XiaoZhi board-local helper
- migrate e-ink display sequencing into the XiaoZhi display implementation
- migrate memo / calendar / packed frame logic into later BLE-facing or board-local interfaces

## Scope

This is a reference snapshot, not the active runtime architecture for the current branch.

Files included here are intentionally limited to migration-relevant source, configuration, and documentation. Build artifacts are not preserved.

# Pad Hooks API v1

Firmware pad interception for Move Everything modules. Allows modules to override the Ableton Move's pad-to-note mapping and pad coloring.

## What It Does

The Move firmware has two internal functions that control pad behavior:

- **`pad_to_note(pad_idx, config)`** — Maps a physical pad press to a MIDI note
- **`note_to_color(config, track_slot, in_scale, not_root)`** — Classifies a note for pad LED coloring (root / in-scale / off-scale)

This API hooks these functions at runtime via pattern matching and trampoline-based redirection, allowing any module to provide custom pad behavior.

## Architecture

```
┌──────────────────────────────────────────┐
│ Module (e.g. PitchGrid)                  │
│                                          │
│  pad_to_note callback:                   │
│    (pad, row, col, octave) → MIDI note   │
│                                          │
│  classify callback:                      │
│    (pad, note) → root/in-scale/off-scale │
└──────────────┬───────────────────────────┘
               │ pad_hooks_api_v1.h
┌──────────────▼───────────────────────────┐
│ pad_hooks.c                              │
│  - Firmware needle patterns              │
│  - Trampoline hook functions             │
│  - Grid decomposition (row/col)          │
│  - Octave resolution (getMelodicOctave)  │
│  - Refcounting for multi-instance safety │
└──────────────┬───────────────────────────┘
               │
┌──────────────▼───────────────────────────┐
│ hook_engine.c                            │
│  - Generic ARM64 pattern scanner         │
│  - SIGSEGV-safe memory scanning          │
│  - Trampoline allocation (mmap RWX)      │
│  - Icache coherency                      │
└──────────────────────────────────────────┘
```

## API Reference

```c
#include "pad_hooks_api_v1.h"

/* Callbacks */
typedef int (*pad_hooks_note_fn)(int pad_idx, int row, int col,
                                  int octave_index, void *context);
typedef int (*pad_hooks_classify_fn)(int pad_idx, int note, void *context);

/* Install/remove */
int  pad_hooks_install(const pad_hooks_callbacks_t *cb,
                       pad_hooks_log_fn log, pad_hooks_log_fn diag);
void pad_hooks_remove(pad_hooks_log_fn log);
```

### pad_to_note callback

Called when a pad is pressed. Parameters:

| Parameter | Description |
|-----------|-------------|
| `pad_idx` | Physical pad index (0-31) |
| `row` | Grid row (0-3, bottom to top) |
| `col` | Grid column (0-7, left to right) |
| `octave_index` | Firmware's current octave setting (0-8) |
| `context` | Opaque pointer from install |

Return a MIDI note (0-127) or -1 to fall back to firmware default.

### classify callback

Called after pad_to_note to determine pad LED color. Return:

| Value | Meaning |
|-------|---------|
| 0 | Root note |
| 1 | In scale |
| 2 | Off scale / chromatic |

## Firmware Compatibility

The needle patterns in `pad_hooks.c` are specific to the **MoveOriginal** binary (Ableton Move firmware). If the firmware is updated, the patterns may need to be updated.

The hook engine requires exactly one match per needle in executable memory. If the firmware changes such that the patterns no longer match (or match multiple times), the hooks will safely fail to install and the module will fall back to default behavior.

## Move Everything Integration

This API is designed for adoption by the Move Everything framework. The intended integration path:

1. Move `pad_hooks/` into `move-anything/src/host/`
2. Add `pad_hooks_install`/`pad_hooks_remove` to the host API (e.g. `plugin_api_v2.h`)
3. Modules request pad hooks via a capability flag in `module.json`
4. The framework manages hook lifecycle (install on module load, remove on unload)
5. Only one module may hold pad hooks at a time (enforced by refcounting)

This would allow any module — not just PitchGrid — to provide custom pad layouts and coloring.

## Files

| File | Description |
|------|-------------|
| `pad_hooks_api_v1.h` | Public API header |
| `pad_hooks.c` | Implementation (firmware-specific glue) |
| `hook_engine.h` | Generic hook engine header |
| `hook_engine.c` | Generic ARM64 pattern scan + trampoline engine |

/*
 * Pad Hooks API v1 — Firmware pad interception for Move Everything modules
 *
 * Allows modules to override the firmware's pad-to-note mapping and pad
 * coloring (note_to_color classification). Only one module may hold pad
 * hooks at a time.
 *
 * This API is designed for adoption by the Move Everything framework.
 * Currently implemented via runtime firmware hooking (hook_engine).
 *
 * Usage:
 *   1. Define pad_to_note and classify callbacks
 *   2. Call pad_hooks_install() during module init
 *   3. Call pad_hooks_remove() during module teardown
 *
 * Grid layout: 32 pads in a 4x8 grid.
 *   pad_idx = row * 8 + col  (row 0 = bottom, col 0 = left)
 */

#ifndef PAD_HOOKS_API_V1_H
#define PAD_HOOKS_API_V1_H

#include <stdint.h>

/* Printf-like logging callback */
typedef void (*pad_hooks_log_fn)(const char *fmt, ...);

/*
 * Callback: compute MIDI note for a pad press.
 *
 * pad_idx:       0-31 (physical pad index)
 * row:           0-3  (bottom to top)
 * col:           0-7  (left to right)
 * octave_index:  Firmware's current octave setting (0-8)
 * context:       Opaque pointer passed through from install
 *
 * Return: MIDI note 0-127, or -1 to use firmware default.
 */
typedef int (*pad_hooks_note_fn)(int pad_idx, int row, int col,
                                  int octave_index, void *context);

/*
 * Callback: classify a pad for coloring.
 *
 * pad_idx:  0-31 (physical pad index)
 * note:     The MIDI note assigned by the pad_to_note callback
 * context:  Opaque pointer passed through from install
 *
 * Return: 0=root, 1=in-scale, 2=off-scale
 */
typedef int (*pad_hooks_classify_fn)(int pad_idx, int note, void *context);

/*
 * Callback table — register your pad override functions here.
 */
typedef struct {
    pad_hooks_note_fn      pad_to_note;  /* Required */
    pad_hooks_classify_fn  classify;     /* Required */
    void                  *context;      /* Opaque user pointer */
} pad_hooks_callbacks_t;

/*
 * Install pad hooks. Only one module may hold hooks at a time.
 * Uses refcounting for multi-instance safety.
 *
 * log:  File-based logging (install/remove messages, not hot path)
 * diag: Lock-free ring buffer logging (hot-path hook callbacks)
 *
 * Returns 0 on success, -1 on failure.
 */
int pad_hooks_install(const pad_hooks_callbacks_t *cb,
                      pad_hooks_log_fn log,
                      pad_hooks_log_fn diag);

/*
 * Remove pad hooks, restore original firmware behavior.
 * Safe to call multiple times (refcounted).
 */
void pad_hooks_remove(pad_hooks_log_fn log);

/*
 * Per-pad classification buffer, updated by the pad_to_note hook.
 * Indexed by pad_idx (0-31). Values: 0=root, 1=in-scale, 2=off-scale.
 * Read by the note_to_color hook to pass classification to firmware.
 */
extern uint8_t pad_hooks_pad_class[32];

#endif /* PAD_HOOKS_API_V1_H */

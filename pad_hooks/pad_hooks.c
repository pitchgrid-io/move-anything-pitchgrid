/*
 * Pad Hooks — Firmware pad interception implementation
 *
 * Hooks the Move firmware's pad_to_note() and note_to_color() functions
 * using the generic hook_engine. Decomposes pad grid coordinates and
 * delegates to module-provided callbacks via pad_hooks_api_v1.
 *
 * Firmware-specific details:
 *   - Needle patterns match MoveOriginal (Ableton Move firmware)
 *   - getMelodicOctave() is resolved by decoding a BL instruction
 *   - Drum tracks (track_slot >= 64) pass through unmodified
 *   - Grid is 4 rows x 8 cols, 32 pads total
 */

#include "pad_hooks_api_v1.h"
#include "hook_engine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* ── Firmware needle patterns ────────────────────────────────────── */

/*
 * First 11 instructions (44 bytes) of pad_to_note() in MoveOriginal.
 * Stops before the first BL (PC-relative call).
 */
static const uint8_t PAD_TO_NOTE_NEEDLE[] = {
    0xff, 0x03, 0x02, 0xd1,  /* sub  sp, sp, #0x80        */
    0xfd, 0x7b, 0x02, 0xa9,  /* stp  x29, x30, [sp, #32]  */
    0xfb, 0x1b, 0x00, 0xf9,  /* str  x27, [sp, #48]       */
    0xfa, 0x67, 0x04, 0xa9,  /* stp  x26, x25, [sp, #64]  */
    0xf8, 0x5f, 0x05, 0xa9,  /* stp  x24, x23, [sp, #80]  */
    0xf6, 0x57, 0x06, 0xa9,  /* stp  x22, x21, [sp, #96]  */
    0xf4, 0x4f, 0x07, 0xa9,  /* stp  x20, x19, [sp, #112] */
    0xfd, 0x83, 0x00, 0x91,  /* add  x29, sp, #0x20       */
    0xf4, 0x03, 0x00, 0xaa,  /* mov  x20, x0              */
    0xe0, 0x03, 0x01, 0xaa,  /* mov  x0, x1               */
    0xf5, 0x03, 0x01, 0xaa,  /* mov  x21, x1              */
};

/*
 * First 7 instructions (28 bytes) of note_to_color() in MoveOriginal.
 */
static const uint8_t NOTE_TO_COLOR_NEEDLE[] = {
    0xfd, 0x7b, 0xbe, 0xa9,  /* stp  x29, x30, [sp, #-0x20]! */
    0xf4, 0x4f, 0x01, 0xa9,  /* stp  x20, x19, [sp, #0x10]   */
    0xfd, 0x03, 0x00, 0x91,  /* mov  x29, sp                  */
    0x34, 0x1c, 0x00, 0x12,  /* and  w20, w1, #0xff           */
    0xe1, 0x03, 0x02, 0x2a,  /* mov  w1, w2                   */
    0xe2, 0x03, 0x03, 0x2a,  /* mov  w2, w3                   */
    0xf3, 0x03, 0x03, 0x2a,  /* mov  w19, w3                  */
};

/* ── Firmware function pointer types ─────────────────────────────── */

typedef uint64_t (*fw_pad_to_note_fn)(uint64_t pad_idx, uint64_t config);
typedef uint64_t (*fw_note_to_color_fn)(uint64_t config, uint32_t track_slot,
                                         uint32_t in_scale, uint32_t not_root);
typedef int (*fw_get_octave_fn)(uint64_t config);

/* ── Static state ────────────────────────────────────────────────── */

static hook_handle_t s_pad_handle;
static hook_handle_t s_color_handle;
static int s_refcount = 0;

static fw_get_octave_fn s_get_octave = NULL;

static pad_hooks_log_fn     s_log  = NULL;
static pad_hooks_log_fn     s_diag = NULL;
static pad_hooks_callbacks_t s_cb  = {0};

/* Hook call counters (atomic, for diagnostics) */
static volatile uint32_t s_pad_calls   = 0;
static volatile uint32_t s_color_calls = 0;

/* Shared classification buffer */
uint8_t pad_hooks_pad_class[32];

/* ── Helpers ─────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t s_start_ms = 0;

/*
 * Decode an ARM64 BL instruction at `addr` and return the absolute target.
 */
static void *decode_bl_target(const uint8_t *addr) {
    uint32_t insn;
    memcpy(&insn, addr, 4);

    if ((insn >> 26) != 0x25)
        return NULL;

    int32_t offset = (int32_t)(insn & 0x03FFFFFF);
    if (offset & (1 << 25))
        offset |= (int32_t)0xFC000000;

    return (void *)(addr + (int64_t)offset * 4);
}

/* ── Hook trampoline functions ───────────────────────────────────── */

#define DRUM_TRACK_THRESHOLD 64

static uint64_t hook_pad_to_note(uint64_t pad_idx, uint64_t config) {
    fw_pad_to_note_fn original = (fw_pad_to_note_fn)s_pad_handle.original_fn;
    uint32_t n = __atomic_fetch_add(&s_pad_calls, 1, __ATOMIC_RELAXED);

    /* Call original for firmware side effects (bookkeeping) */
    original(pad_idx, config);

    /* Read firmware's current octave index (0-8) */
    int octave_index = 0;
    if (s_get_octave)
        octave_index = s_get_octave(config);

    /* Decompose pad grid: 4 rows x 8 cols */
    int pad = (int)pad_idx;
    int col = pad % 8;
    int row = pad / 8;

    /* Delegate to module callback */
    int note = -1;
    if (s_cb.pad_to_note)
        note = s_cb.pad_to_note(pad, row, col, octave_index, s_cb.context);

    if (note < 0) note = 0;
    if (note > 127) note = 127;

    /* Classify for coloring */
    int cls = 2;
    if (s_cb.classify)
        cls = s_cb.classify(pad, note, s_cb.context);
    pad_hooks_pad_class[pad % 32] = (uint8_t)cls;

    /* Diagnostic logging: first 8 calls, then sparse */
    if (s_diag && (n < 8 || (n % 100000 == 0))) {
        s_diag("[pad] #%u idx=%d note=%d cls=%d oct=%d t=%llums",
               n, pad, note, cls, octave_index,
               (unsigned long long)(now_ms() - s_start_ms));
    }

    return (uint64_t)(uint32_t)note | (1ULL << 32);
}

static uint64_t hook_note_to_color(uint64_t config, uint32_t track_slot,
                                    uint32_t in_scale, uint32_t not_root) {
    fw_note_to_color_fn original = (fw_note_to_color_fn)s_color_handle.original_fn;
    __atomic_fetch_add(&s_color_calls, 1, __ATOMIC_RELAXED);

    /* Drum tracks pass through unmodified */
    if (track_slot >= DRUM_TRACK_THRESHOLD)
        return original(config, track_slot, in_scale, not_root);

    /* Map pad index to classification from pad_hooks_pad_class[] */
    static uint32_t pad_count = 0;
    uint32_t idx = pad_count % 32;
    pad_count++;

    return original(config, track_slot, 0, pad_hooks_pad_class[idx]);
}

/* ── Public API ──────────────────────────────────────────────────── */

int pad_hooks_install(const pad_hooks_callbacks_t *cb,
                      pad_hooks_log_fn log,
                      pad_hooks_log_fn diag) {
    s_log  = log;
    s_diag = diag;

    if (s_refcount > 0) {
        s_refcount++;
        if (s_log) s_log("[pad_hooks] Already installed (refcount=%d)\n", s_refcount);
        return 0;
    }

    if (!cb || !cb->pad_to_note || !cb->classify) {
        if (s_log) s_log("[pad_hooks] ERROR: callbacks required\n");
        return -1;
    }

    s_cb = *cb;
    s_start_ms = now_ms();
    memset(pad_hooks_pad_class, 2, sizeof(pad_hooks_pad_class));

    /* Install pad_to_note hook */
    hook_spec_t pad_spec = {
        .needle     = PAD_TO_NOTE_NEEDLE,
        .needle_len = sizeof(PAD_TO_NOTE_NEEDLE),
        .hook_fn    = (void *)hook_pad_to_note,
        .name       = "pad_to_note",
    };

    int rc = hook_install(&pad_spec, &s_pad_handle, log);
    if (rc != 0) {
        if (s_log) s_log("[pad_hooks] pad_to_note hook FAILED\n");
        return -1;
    }

    /* Resolve getMelodicOctave by decoding BL at offset +44 */
    void *bl_target = decode_bl_target(s_pad_handle.target_addr + 44);
    if (bl_target) {
        s_get_octave = (fw_get_octave_fn)bl_target;
        if (s_log) s_log("[pad_hooks] Resolved getMelodicOctave at %p\n", bl_target);
    } else {
        if (s_log) s_log("[pad_hooks] WARNING: could not decode BL at +44\n");
    }

    /* Install note_to_color hook */
    hook_spec_t color_spec = {
        .needle     = NOTE_TO_COLOR_NEEDLE,
        .needle_len = sizeof(NOTE_TO_COLOR_NEEDLE),
        .hook_fn    = (void *)hook_note_to_color,
        .name       = "note_to_color",
    };

    rc = hook_install(&color_spec, &s_color_handle, log);
    if (rc != 0) {
        if (s_log) s_log("[pad_hooks] note_to_color hook FAILED (pad_to_note still active)\n");
    }

    s_refcount = 1;
    if (s_log) s_log("[pad_hooks] Hooks installed\n");
    return 0;
}

void pad_hooks_remove(pad_hooks_log_fn log) {
    s_refcount--;
    if (s_refcount > 0) {
        if (log) log("[pad_hooks] Hooks kept alive (refcount=%d)\n", s_refcount);
        return;
    }

    s_refcount = 0;
    hook_remove(&s_color_handle, log);
    hook_remove(&s_pad_handle, log);
    s_get_octave = NULL;
    memset(&s_cb, 0, sizeof(s_cb));

    if (log) log("[pad_hooks] Hooks removed\n");
}

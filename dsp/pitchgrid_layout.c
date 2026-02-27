/*
 * PitchGrid Pad Layout — Scale-aware pad mapping and classification
 *
 * Implements the pad_hooks_api_v1 callbacks for PitchGrid-specific
 * pad-to-note mapping (chromatic and in-key layouts) and pad coloring
 * (root / in-scale / off-scale classification).
 *
 * Reads scale data (color pattern, equave size, etc.) from the scale
 * module's globals.
 */

#include "pitchgrid_layout.h"
#include "pitchgrid_scale.h"
#include "pad_hooks_api_v1.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define PG_ROOT_NOTE 60  /* Root at MIDI note 60 (mapped scale center) */

/* ── Static state ─────────────────────────────────────────────────── */

static pg_log_fn s_log = NULL;

/* Scale-derived lookup tables (rebuilt by pg_layout_update_tables) */
static int s_degree_to_chromatic[PG_MAX_SCALE_DEGS];
static int s_lowest_root = 0;

/* ── Extern globals ───────────────────────────────────────────────── */

int     pg_layout_type = PG_LAYOUT_CHROMATIC;
int     pg_row_offset  = 5;

/* ── Helpers ──────────────────────────────────────────────────────── */

/*
 * Classify a MIDI note relative to the current scale.
 * Returns: 0=root, 1=in-scale, 2=off-scale.
 */
static int classify_note(int note) {
    int eq = pg_equave_size;
    if (eq <= 0) return 2;
    int offset = ((note - PG_ROOT_NOTE) % eq + eq) % eq;
    if (offset >= PG_MAX_EQUAVE) return 2;
    return pg_color_pattern[offset];
}

/* ── pad_hooks callbacks ─────────────────────────────────────────── */

static int cb_pad_to_note(int pad_idx, int row, int col,
                           int octave_index, void *context) {
    (void)context;
    int note;

    if (pg_layout_type == PG_LAYOUT_IN_KEY) {
        /* In-key: rows are equaves, cols are scale degrees.
         * If col >= scale_degrees, wrap into next equave(s). */
        int extra_equaves = col / pg_scale_degrees;
        int deg = col % pg_scale_degrees;
        note = s_lowest_root
             + (octave_index + row + extra_equaves) * pg_equave_size
             + s_degree_to_chromatic[deg];
    } else {
        /* Chromatic: pad col 3 = root at oct=0. */
        int base = s_lowest_root - 3;
        note = base
             + octave_index * pg_equave_size
             + row * pg_row_offset
             + col;
    }

    /* Clamp to MIDI range */
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    return note;
}

static int cb_classify(int pad_idx, int note, void *context) {
    (void)pad_idx;
    (void)context;
    return classify_note(note);
}

/* ── Public API ───────────────────────────────────────────────────── */

int pg_layout_install(pg_log_fn log, pg_log_fn diag_log) {
    s_log = log;

    pad_hooks_callbacks_t cb = {
        .pad_to_note = cb_pad_to_note,
        .classify    = cb_classify,
        .context     = NULL,
    };

    return pad_hooks_install(&cb, log, diag_log);
}

void pg_layout_remove(pg_log_fn log) {
    pad_hooks_remove(log);
}

void pg_layout_update_tables(void) {
    /* Build degree_to_chromatic[] from color pattern */
    int deg = 0;
    for (int i = 0; i < pg_equave_size && i < PG_MAX_EQUAVE && deg < PG_MAX_SCALE_DEGS; i++) {
        if (pg_color_pattern[i] <= 1) {  /* 0=root or 1=scale */
            s_degree_to_chromatic[deg] = i;
            deg++;
        }
    }

    /* Compute lowest root: go down from root in equave steps */
    s_lowest_root = PG_ROOT_NOTE;
    for (int i = 0; i < 5; i++) {
        int next = s_lowest_root - pg_equave_size;
        if (next < 0) break;
        s_lowest_root = next;
    }

    if (s_log) {
        s_log("[layout] tables updated: equave=%d degrees=%d lowest_root=%d\n",
              pg_equave_size, pg_scale_degrees, s_lowest_root);
    }
}

/* ── Parameter handling ───────────────────────────────────────────── */

int pg_layout_set_param(const char *key, const char *val) {
    if (!key || !val) return 0;

    if (strcmp(key, "layout_type") == 0) {
        if (strcmp(val, "in-key") == 0)
            pg_layout_type = PG_LAYOUT_IN_KEY;
        else
            pg_layout_type = PG_LAYOUT_CHROMATIC;
        return 1;
    }
    if (strcmp(key, "row_offset") == 0) {
        int v = atoi(val);
        if (v >= 3 && v <= 9) pg_row_offset = v;
        return 1;
    }
    return 0;
}

int pg_layout_get_param(const char *key, char *buf, int buf_len) {
    if (!key || !buf) return 0;

    if (strcmp(key, "layout_type") == 0) {
        snprintf(buf, buf_len, "%s",
                 pg_layout_type == PG_LAYOUT_IN_KEY ? "in-key" : "chromatic");
        return 1;
    }
    if (strcmp(key, "row_offset") == 0) {
        snprintf(buf, buf_len, "%d", pg_row_offset);
        return 1;
    }
    return 0;
}

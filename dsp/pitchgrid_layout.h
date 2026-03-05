/*
 * PitchGrid Pad Layout — Scale-aware pad mapping and classification
 *
 * Implements pad_hooks_api_v1 callbacks for PitchGrid-specific
 * chromatic and in-key pad layouts, plus pad coloring based on
 * the current scale's color pattern.
 */

#ifndef PITCHGRID_LAYOUT_H
#define PITCHGRID_LAYOUT_H

#include <stdint.h>

#define PG_LAYOUT_CHROMATIC 0
#define PG_LAYOUT_IN_KEY    1

/* ── Shared layout state ───────────────────────────────────────── */

extern int pg_layout_type;    /* LAYOUT_CHROMATIC or LAYOUT_IN_KEY */
extern int pg_row_offset;     /* chromatic steps between rows (0-8) */

/* ── Functions ─────────────────────────────────────────────────── */

typedef void (*pg_log_fn)(const char *fmt, ...);

/* Install pad hooks with PitchGrid layout callbacks.
 * log: file-based logging (install/remove messages).
 * diag_log: lock-free ring buffer logging (hot-path callbacks).
 * Returns 0 on success, -1 on failure. */
int pg_layout_install(pg_log_fn log, pg_log_fn diag_log);

/* Remove pad hooks, restore original firmware code. */
void pg_layout_remove(pg_log_fn log);

/* Rebuild degree_to_chromatic[] and lowest_root from current scale data.
 * Call after pg_scale_recalc(). */
void pg_layout_update_tables(void);

/* Route set_param/get_param for layout keys.
 * Returns 1 if key was handled, 0 otherwise. */
int pg_layout_set_param(const char *key, const char *val);
int pg_layout_get_param(const char *key, char *buf, int buf_len);

#endif /* PITCHGRID_LAYOUT_H */

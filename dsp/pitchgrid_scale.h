/*
 * PitchGrid Scale Engine — Scalatrix MOS + Scale Generation
 *
 * Creates a MOS from (depth, stretch, skew, mode), generates a 128-node
 * mapped scale, builds the cyclic color pattern, and fills the log2freq
 * lookup table.
 */

#ifndef PITCHGRID_SCALE_H
#define PITCHGRID_SCALE_H

#include <stdint.h>

#define PG_MAX_EQUAVE     64
#define PG_MAX_SCALE_DEGS 32

/* log2(261.6255653) — C4 in standard 12-TET */
#define PG_LOG2_ROOT_FREQ 8.031359713524660
#define PG_ROOT_FREQ      261.6255653

/* ── Shared scale data (read by layout + MPE modules) ──────────── */

extern uint8_t pg_color_pattern[PG_MAX_EQUAVE]; /* 0=root, 1=scale, 2=off */
extern int     pg_equave_size;                  /* chromatic steps per equave */
extern int     pg_scale_degrees;                /* in-scale notes per equave */
extern double  pg_log2freqs[128];               /* log2(Hz) per MIDI note */

/* ── Scale parameters ──────────────────────────────────────────── */

extern int    pg_depth;           /* Stern-Brocot tree depth (1-10) */
extern int    pg_extra_depth;     /* additional depth for chromatic grid (0-3) */
extern double pg_stretch;         /* equave in cents (100-2400) */
extern double pg_skew;            /* generator (0.0-1.0) */
extern int    pg_mode;            /* mode index (0-20, clamped) */
extern double pg_root_freq_cents; /* root freq offset in cents (-1200..+1200) */
extern int    pg_repetitions;     /* periods per equave (1 = single-period) */
extern int    pg_dirty;           /* 1 if params changed, needs recalc */

/* ── Functions ─────────────────────────────────────────────────── */

/* Logging callback type (provided by main module) */
typedef void (*pg_log_fn)(const char *fmt, ...);

/* Recalculate MOS, scale, pattern, and log2freq table.
 * Call when pg_dirty is set. */
void pg_scale_recalc(pg_log_fn log);

/* Free scalatrix objects. Call on last instance destroy. */
void pg_scale_free(void);

/* Route set_param/get_param for scale keys.
 * Returns 1 if key was handled, 0 otherwise. */
int pg_scale_set_param(const char *key, const char *val);
int pg_scale_get_param(const char *key, char *buf, int buf_len);

#endif /* PITCHGRID_SCALE_H */

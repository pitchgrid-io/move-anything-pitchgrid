/*
 * PitchGrid Scale Engine — Scalatrix MOS + Scale Generation
 *
 * Two-MOS approach:
 *   scale_mos     (depth)              — defines scale degrees
 *   chromatic_mos (depth + extra_depth) — defines the chromatic grid
 *
 * The mapped scale is generated from the chromatic MOS.
 * Each node's natural_coord is mapped from chromatic→scale space
 * to classify as root / in-scale / off-scale.
 */

#include "pitchgrid_scale.h"
#include "scalatrix/c_api.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Globals ─────────────────────────────────────────────────────── */

uint8_t pg_color_pattern[PG_MAX_EQUAVE];
int     pg_equave_size   = 0;
int     pg_scale_degrees = 0;
double  pg_log2freqs[128];

int    pg_depth           = 3;
int    pg_extra_depth     = 1;
double pg_stretch         = 1200.0;
double pg_skew            = 0.585;
int    pg_mode            = 0;
double pg_root_freq_cents = 0.0;
int    pg_repetitions     = 1;
int    pg_dirty           = 1;  /* start dirty so first recalc runs */

static scalatrix_mos_t   *s_scale_mos     = NULL;  /* scale degrees */
static scalatrix_mos_t   *s_chromatic_mos = NULL;  /* chromatic grid */
static scalatrix_scale_t *s_scale         = NULL;  /* 128-node mapped scale */

/* ── Color pattern via mapFromMOS ──────────────────────────────── */

/*
 * Iterate first equave of the mapped scale (nodes 60..60+n0-1).
 * For each node, map its chromatic natural_coord into scale space,
 * then classify: root (0), in-scale (1), off-scale (2).
 */
static void build_color_pattern(pg_log_fn log) {
    int n = pg_equave_size;  /* full equave (all periods) */

    for (int i = 0; i < n && i < PG_MAX_EQUAVE; i++) {
        scalatrix_node node;
        if (scalatrix_scale_get_node(s_scale, 60 + i, &node) != 0) {
            pg_color_pattern[i] = 2;
            log("[color] i=%d: node fetch failed\n", i);
            continue;
        }

        /* Map from chromatic coordinate space → scale coordinate space */
        scalatrix_vec2i sc = scalatrix_mos_map_from_mos(
            s_scale_mos, s_chromatic_mos, node.natural_coord);

        int degree     = scalatrix_mos_node_scale_degree(s_scale_mos, sc);
        int in_scale   = scalatrix_mos_node_in_scale(s_scale_mos, sc);
        int accidental = scalatrix_mos_node_accidental(s_scale_mos, sc);

        if (degree == 0 && accidental == 0)
            pg_color_pattern[i] = 0;  /* root */
        else if (in_scale)
            pg_color_pattern[i] = 1;  /* in-scale */
        else
            pg_color_pattern[i] = 2;  /* off-scale (chromatic) */
    }

    /* Log pattern */
    log("[scale] pattern (%d steps): ", n);
    for (int i = 0; i < n; i++)
        log("%d", pg_color_pattern[i]);
    log("\n");
}

/* ── Scale recalculation ─────────────────────────────────────────── */

void pg_scale_recalc(pg_log_fn log) {
    /* Free old objects */
    if (s_scale)         { scalatrix_scale_free(s_scale);       s_scale = NULL; }
    if (s_chromatic_mos) { scalatrix_mos_free(s_chromatic_mos); s_chromatic_mos = NULL; }
    if (s_scale_mos)     { scalatrix_mos_free(s_scale_mos);     s_scale_mos = NULL; }

    double stretch_log2 = pg_stretch / 1200.0;
    int total_depth = pg_depth + pg_extra_depth;

    /* ── Scale MOS (defines scale degrees) ─────────────────────── */

    /* Create with mode=0 first to determine n for mode clamping */
    s_scale_mos = scalatrix_mos_from_g(
        pg_depth, 0, pg_skew, stretch_log2, pg_repetitions);
    if (!s_scale_mos) {
        log("[scale] ERROR: failed to create scale MOS\n");
        return;
    }

    int n = scalatrix_mos_n(s_scale_mos);
    int clamped_mode = pg_mode;
    if (clamped_mode >= n) clamped_mode = n - 1;
    if (clamped_mode < 0) clamped_mode = 0;

    /* Recreate with correct mode if needed */
    if (clamped_mode != 0) {
        scalatrix_mos_free(s_scale_mos);
        s_scale_mos = scalatrix_mos_from_g(
            pg_depth, clamped_mode, pg_skew, stretch_log2, pg_repetitions);
        if (!s_scale_mos) {
            log("[scale] ERROR: failed to create scale MOS mode=%d\n", clamped_mode);
            return;
        }
    }

    pg_scale_degrees = scalatrix_mos_n(s_scale_mos);

    int nL = scalatrix_mos_nL(s_scale_mos);
    int nS = scalatrix_mos_nS(s_scale_mos);

    /* ── Chromatic MOS (defines display grid) ──────────────────── */

    s_chromatic_mos = scalatrix_mos_from_g(
        total_depth, 0, pg_skew, stretch_log2, pg_repetitions);
    if (!s_chromatic_mos) {
        log("[scale] ERROR: failed to create chromatic MOS\n");
        return;
    }

    int chromatic_n0 = scalatrix_mos_n0(s_chromatic_mos);
    pg_equave_size   = scalatrix_mos_n(s_chromatic_mos);  /* full equave = n */

    log("[scale] scale: %dL%ds mode=%d depth=%d degrees=%d\n",
        nL, nS, clamped_mode, pg_depth, pg_scale_degrees);
    log("[scale] chromatic: depth=%d n0=%d n=%d\n",
        total_depth, chromatic_n0, pg_equave_size);

    /* ── Generate mapped scale from chromatic MOS ──────────────── */

    double root_freq = PG_ROOT_FREQ * pow(2.0, pg_root_freq_cents / 1200.0);
    double log2_root = PG_LOG2_ROOT_FREQ + pg_root_freq_cents / 1200.0;

    s_scale = scalatrix_mos_generate_mapped_scale(
        s_chromatic_mos, pg_equave_size, (double)clamped_mode,
        root_freq, 128, 60);

    if (!s_scale) {
        log("[scale] ERROR: failed to generate mapped scale\n");
        return;
    }

    /* ── Build color pattern ───────────────────────────────────── */

    build_color_pattern(log);

    /* ── Fill log2freq table ───────────────────────────────────── */

    for (int i = 0; i < 128; i++) {
        scalatrix_node node;
        if (scalatrix_scale_get_node(s_scale, i, &node) == 0) {
            pg_log2freqs[i] = log2_root + node.tuning_coord.x;
        } else {
            /* Fallback: standard 12-TET */
            pg_log2freqs[i] = log2_root + (i - 60) / 12.0;
        }
    }

    pg_dirty = 0;

    log("[scale] recalc complete: equave=%d degrees=%d\n",
        pg_equave_size, pg_scale_degrees);
}

void pg_scale_free(void) {
    if (s_scale)         { scalatrix_scale_free(s_scale);       s_scale = NULL; }
    if (s_chromatic_mos) { scalatrix_mos_free(s_chromatic_mos); s_chromatic_mos = NULL; }
    if (s_scale_mos)     { scalatrix_mos_free(s_scale_mos);     s_scale_mos = NULL; }
}

/* ── Parameter handling ──────────────────────────────────────────── */

int pg_scale_set_param(const char *key, const char *val) {
    if (!key || !val) return 0;

    if (strcmp(key, "depth") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= 10) { pg_depth = v; pg_dirty = 1; }
        return 1;
    }
    if (strcmp(key, "stretch") == 0) {
        double v = atof(val);
        if (v >= 100.0 && v <= 2400.0) { pg_stretch = v; pg_dirty = 1; }
        return 1;
    }
    if (strcmp(key, "skew") == 0) {
        double v = atof(val);
        if (v >= 0.0 && v <= 1.0) { pg_skew = v; pg_dirty = 1; }
        return 1;
    }
    if (strcmp(key, "mode") == 0) {
        int v = atoi(val);
        if (v >= 0 && v <= 20) { pg_mode = v; pg_dirty = 1; }
        return 1;
    }
    if (strcmp(key, "root_freq_cents") == 0) {
        double v = atof(val);
        if (v >= -1200.0 && v <= 1200.0) { pg_root_freq_cents = v; pg_dirty = 1; }
        return 1;
    }
    return 0;
}

int pg_scale_get_param(const char *key, char *buf, int buf_len) {
    if (!key || !buf) return 0;

    if (strcmp(key, "depth") == 0)
        { snprintf(buf, buf_len, "%d", pg_depth); return 1; }
    if (strcmp(key, "stretch") == 0)
        { snprintf(buf, buf_len, "%.2f", pg_stretch); return 1; }
    if (strcmp(key, "skew") == 0)
        { snprintf(buf, buf_len, "%.4f", pg_skew); return 1; }
    if (strcmp(key, "mode") == 0)
        { snprintf(buf, buf_len, "%d", pg_mode); return 1; }
    if (strcmp(key, "root_freq_cents") == 0)
        { snprintf(buf, buf_len, "%.1f", pg_root_freq_cents); return 1; }
    return 0;
}

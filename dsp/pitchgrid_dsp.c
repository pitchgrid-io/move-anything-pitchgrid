/*
 * PitchGrid DSP — Entry Point
 *
 * Thin glue that ties together the scale, layout, and MPE modules.
 * Implements the midi_fx_api_v1 interface.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#include "pitchgrid_scale.h"
#include "pitchgrid_layout.h"
#include "pitchgrid_mpe.h"
#include "pitchgrid_presets.h"

/* ── Constants ────────────────────────────────────────────────────── */

#define LOG_PATH "/data/UserData/move-anything/pitchgrid.log"

/* ── Logging ──────────────────────────────────────────────────────── */

static FILE *g_log = NULL;

static void logf_file(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

/* ── Diagnostic ring buffer (hot-path safe) ──────────────────────── */

#define DIAG_RING_SIZE 256
#define DIAG_MSG_LEN   120

typedef struct {
    char msg[DIAG_MSG_LEN];
} diag_entry_t;

static diag_entry_t g_diag_ring[DIAG_RING_SIZE];
static volatile uint32_t g_diag_head = 0;
static uint32_t g_diag_tail = 0;

static void diag_log(const char *fmt, ...) {
    uint32_t slot = __atomic_fetch_add(&g_diag_head, 1, __ATOMIC_RELAXED)
                    % DIAG_RING_SIZE;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_diag_ring[slot].msg, DIAG_MSG_LEN, fmt, ap);
    va_end(ap);
}

static void drain_diag_ring(void) {
    if (!g_log) return;
    uint32_t head = __atomic_load_n(&g_diag_head, __ATOMIC_ACQUIRE);
    if (head - g_diag_tail > DIAG_RING_SIZE)
        g_diag_tail = head - DIAG_RING_SIZE;
    while (g_diag_tail != head) {
        uint32_t slot = g_diag_tail % DIAG_RING_SIZE;
        fprintf(g_log, "%s\n", g_diag_ring[slot].msg);
        g_diag_tail++;
    }
    fflush(g_log);
}

/* ── Presets ──────────────────────────────────────────────────────── */

static int g_current_preset = 0;

static void load_preset(int idx) {
    if (idx < 0 || idx >= PG_PRESET_COUNT) return;
    const pg_preset_t *p = &pg_presets[idx];
    pg_depth           = p->depth;
    pg_extra_depth     = p->extra_depth;
    pg_stretch         = p->stretch;
    pg_skew            = p->skew;
    pg_mode            = p->mode;
    pg_root_freq_cents = p->root_freq_cents;
    pg_repetitions     = p->repetitions;
    pg_dirty           = 1;
    g_current_preset   = idx;
}

/* ── JSON helper (minimal, matches Surge pattern) ────────────────── */

static int json_get_number(const char *json, const char *key, double *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = atof(pos);
    return 0;
}

/* ── Chain params (read from module.json, served via get_param) ──── */

#define CHAIN_PARAMS_BUF_SIZE 4096
static char g_chain_params_json[CHAIN_PARAMS_BUF_SIZE] = "";

static void load_chain_params(const char *module_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/module.json", module_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Find "chain_params" array and extract it */
    const char *start = strstr(buf, "\"chain_params\"");
    if (!start) return;
    start = strchr(start, '[');
    if (!start) return;

    /* Find matching ] using bracket depth */
    const char *end = start + 1;
    int depth = 1;
    while (*end && depth > 0) {
        if (*end == '[') depth++;
        else if (*end == ']') depth--;
        end++;
    }

    int len = (int)(end - start);
    if (len > 0 && len < CHAIN_PARAMS_BUF_SIZE) {
        memcpy(g_chain_params_json, start, len);
        g_chain_params_json[len] = '\0';
    }
}

/* ── Instance ─────────────────────────────────────────────────────── */

typedef struct {
    pg_mpe_state_t mpe;
} pitchgrid_instance_t;

static int g_instance_count = 0;
static const host_api_v1_t *g_host = NULL;

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void *pg_create_instance(const char *module_dir,
                                 const char *config_json) {
    (void)config_json;

    pitchgrid_instance_t *inst = calloc(1, sizeof(pitchgrid_instance_t));
    if (!inst) return NULL;

    /* Open log */
    if (!g_log || g_log == stderr)
        g_log = fopen(LOG_PATH, g_instance_count == 0 ? "w" : "a");
    if (!g_log) g_log = stderr;

    logf_file("=== PitchGrid instance #%d ===\n", g_instance_count + 1);

    if (g_instance_count == 0) {
        /* First instance: load metadata, default preset, install hooks, recalc */
        load_chain_params(module_dir);
        load_preset(0);  /* 12-TET */
        pg_layout_install(logf_file, diag_log);
        pg_scale_recalc(logf_file);
        pg_layout_update_tables();
    }

    pg_mpe_init(&inst->mpe);

    g_instance_count++;
    logf_file("=== Init complete (instances=%d) ===\n", g_instance_count);

    return inst;
}

static void pg_destroy_instance(void *instance) {
    if (!g_log || g_log == stderr)
        g_log = fopen(LOG_PATH, "a");
    if (!g_log) g_log = stderr;

    g_instance_count--;
    logf_file("\n=== Destroying PitchGrid (instances=%d) ===\n",
              g_instance_count);

    if (g_instance_count <= 0) {
        g_instance_count = 0;

        /* Final drain */
        drain_diag_ring();

        pg_layout_remove(logf_file);
        pg_scale_free();

        logf_file("=== All instances destroyed, hooks removed ===\n");
    }

    if (g_instance_count == 0 && g_log && g_log != stderr) {
        fclose(g_log);
        g_log = NULL;
    }

    free(instance);
}

/* ── MIDI processing ──────────────────────────────────────────────── */

static int pg_process_midi(void *instance,
                            const uint8_t *in_msg, int in_len,
                            uint8_t out_msgs[][3], int out_lens[],
                            int max_out) {
    pitchgrid_instance_t *inst = (pitchgrid_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    /* Drain diag ring */
    drain_diag_ring();

    /* Delegate to MPE module */
    return pg_mpe_process_midi(&inst->mpe,
                                in_msg, in_len,
                                out_msgs, out_lens, max_out);
}

static int pg_tick(void *instance,
                    int frames, int sample_rate,
                    uint8_t out_msgs[][3], int out_lens[],
                    int max_out) {
    (void)frames;
    (void)sample_rate;

    pitchgrid_instance_t *inst = (pitchgrid_instance_t *)instance;
    if (!inst) return 0;

    return pg_mpe_tick(&inst->mpe, out_msgs, out_lens, max_out);
}

/* ── Parameters ───────────────────────────────────────────────────── */

static void do_recalc(pitchgrid_instance_t *inst) {
    pg_scale_recalc(logf_file);
    pg_layout_update_tables();
    if (inst) pg_mpe_retune(&inst->mpe);
}

static void pg_set_param(void *instance, const char *key,
                          const char *val) {
    pitchgrid_instance_t *inst = (pitchgrid_instance_t *)instance;
    if (!key || !val) return;

    logf_file("[set_param] key=\"%s\" val=\"%s\"\n", key, val);

    /* State restore (host calls this when loading a set) */
    if (strcmp(key, "state") == 0) {
        double fval;
        /* Restore preset first (sets all scale params to preset values) */
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            load_preset(idx);
        }
        /* Override individual params from saved state */
        if (json_get_number(val, "depth", &fval) == 0) {
            int v = (int)fval;
            if (v >= 1 && v <= 10) pg_depth = v;
        }
        if (json_get_number(val, "extra_depth", &fval) == 0) {
            int v = (int)fval;
            if (v >= 0 && v <= 5) pg_extra_depth = v;
        }
        if (json_get_number(val, "stretch", &fval) == 0) {
            if (fval >= 100.0 && fval <= 2400.0) pg_stretch = fval;
        }
        if (json_get_number(val, "skew", &fval) == 0) {
            if (fval >= 0.0 && fval <= 1.0) pg_skew = fval;
        }
        if (json_get_number(val, "mode", &fval) == 0) {
            int v = (int)fval;
            if (v >= 0 && v <= 20) pg_mode = v;
        }
        if (json_get_number(val, "root_freq_cents", &fval) == 0) {
            if (fval >= -1200.0 && fval <= 1200.0) pg_root_freq_cents = fval;
        }
        if (json_get_number(val, "repetitions", &fval) == 0) {
            int v = (int)fval;
            if (v >= 1 && v <= 10) pg_repetitions = v;
        }
        if (json_get_number(val, "layout_type", &fval) == 0) {
            pg_layout_type = (int)fval;
        }
        if (json_get_number(val, "row_offset", &fval) == 0) {
            int v = (int)fval;
            if (v >= 0 && v <= 8) pg_row_offset = v;
        }
        if (json_get_number(val, "mpe_bend_range", &fval) == 0 && inst) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", (int)fval);
            pg_mpe_set_param(&inst->mpe, "mpe_bend_range", tmp);
        }
        pg_dirty = 1;
        do_recalc(inst);
        logf_file("[state] restored preset=%d depth=%d extra=%d stretch=%.1f "
                  "skew=%.4f mode=%d root=%.1f rep=%d\n",
                  g_current_preset, pg_depth, pg_extra_depth, pg_stretch,
                  pg_skew, pg_mode, pg_root_freq_cents, pg_repetitions);
        return;
    }

    /* Preset loading */
    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        load_preset(idx);
        do_recalc(inst);
        return;
    }

    /* Route to submodules */
    if (pg_scale_set_param(key, val)) {
        if (pg_dirty) do_recalc(inst);
        return;
    }
    if (pg_layout_set_param(key, val)) return;
    if (inst && pg_mpe_set_param(&inst->mpe, key, val)) return;
}

static int pg_get_param(void *instance, const char *key,
                         char *buf, int buf_len) {
    pitchgrid_instance_t *inst = (pitchgrid_instance_t *)instance;
    if (!key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "PitchGrid");
    if (strcmp(key, "preset") == 0)
        return snprintf(buf, buf_len, "%d", g_current_preset);
    if (strcmp(key, "preset_count") == 0)
        return snprintf(buf, buf_len, "%d", PG_PRESET_COUNT);
    if (strcmp(key, "preset_name") == 0)
        return snprintf(buf, buf_len, "%s", pg_presets[g_current_preset].name);

    if (strcmp(key, "chain_params") == 0 && g_chain_params_json[0]) {
        int len = (int)strlen(g_chain_params_json);
        if (len < buf_len) {
            memcpy(buf, g_chain_params_json, len + 1);
            return len;
        }
        return -1;
    }

    /* State serialization (host calls this when saving a set) */
    if (strcmp(key, "state") == 0) {
        int bend = inst ? inst->mpe.mpe_bend_range : 48;
        return snprintf(buf, buf_len,
            "{\"preset\":%d,\"depth\":%d,\"extra_depth\":%d,"
            "\"stretch\":%.2f,\"skew\":%.6f,\"mode\":%d,"
            "\"root_freq_cents\":%.1f,\"repetitions\":%d,"
            "\"layout_type\":%d,\"row_offset\":%d,"
            "\"mpe_bend_range\":%d}",
            g_current_preset, pg_depth, pg_extra_depth,
            pg_stretch, pg_skew, pg_mode,
            pg_root_freq_cents, pg_repetitions,
            pg_layout_type, pg_row_offset, bend);
    }

    /* Route to submodules */
    if (pg_scale_get_param(key, buf, buf_len)) return (int)strlen(buf);
    if (pg_layout_get_param(key, buf, buf_len)) return (int)strlen(buf);
    if (inst && pg_mpe_get_param(&inst->mpe, key, buf, buf_len))
        return (int)strlen(buf);

    return -1;
}

/* ── Plugin entry point ───────────────────────────────────────────── */

static midi_fx_api_v1_t g_api = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = pg_create_instance,
    .destroy_instance = pg_destroy_instance,
    .process_midi     = pg_process_midi,
    .tick             = pg_tick,
    .set_param        = pg_set_param,
    .get_param        = pg_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    if (host && host->log)
        host->log("PitchGrid MIDI FX loaded");
    return &g_api;
}

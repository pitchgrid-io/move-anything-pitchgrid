/*
 * PitchGrid MPE — Channel Allocation + Pitch Bend
 *
 * From mpe_bend_test.c, adapted to read g_log2freqs[] from scale module.
 */

#include "pitchgrid_mpe.h"
#include "pitchgrid_scale.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── MPE note calculation ────────────────────────────────────────── */

/*
 * Given a target log2 frequency, compute the closest standard MIDI note
 * and the 14-bit pitch bend needed to hit that frequency precisely.
 *
 * All arithmetic: +, -, *, / on doubles. No exp/log/trig.
 */
static void calc_mpe_output(double target_log2freq, int bend_range,
                            uint8_t *out_note, int *out_bend14) {
    /* Fractional MIDI note in standard 12-TET */
    double frac_note = (target_log2freq - PG_LOG2_C4) * 12.0 + 60.0;

    /* Closest integer MIDI note */
    int n = (int)(frac_note >= 0.0 ? frac_note + 0.5 : frac_note - 0.5);
    if (n < 0) n = 0;
    if (n > 127) n = 127;

    /* Delta in semitones from that standard note */
    double delta_semitones = frac_note - (double)n;

    /* 14-bit pitch bend: center at 8192, ±8191 = ±bend_range semitones */
    double bend_frac = delta_semitones / (double)bend_range;
    int bend = 8192 + (int)(bend_frac * 8191.0 +
                            (bend_frac >= 0.0 ? 0.5 : -0.5));
    if (bend < 0) bend = 0;
    if (bend > 16383) bend = 16383;

    *out_note = (uint8_t)n;
    *out_bend14 = bend;
}

/* Recalculate pitch bend for a playing channel (for live retuning) */
static void recalc_channel_bend(pg_mpe_state_t *state, int ch_idx) {
    pg_mpe_channel_t *ch = &state->channels[ch_idx];
    if (!ch->is_playing) return;

    double target = pg_log2freqs[ch->in_note];
    double frac_note = (target - PG_LOG2_C4) * 12.0 + 60.0;
    double delta_semitones = frac_note - (double)ch->out_note;

    double bend_frac = delta_semitones / (double)state->mpe_bend_range;
    int bend = 8192 + (int)(bend_frac * 8191.0 +
                            (bend_frac >= 0.0 ? 0.5 : -0.5));
    if (bend < 0) bend = 0;
    if (bend > 16383) bend = 16383;

    ch->out_bend14 = bend;
}

/* ── Channel stack helpers ───────────────────────────────────────── */

static void stack_remove(int *stack, int num, int ch) {
    for (int i = 0; i < num; i++) {
        if (stack[i] == ch) {
            for (int j = i; j < num - 1; j++)
                stack[j] = stack[j + 1];
            return;
        }
    }
}

static void stack_move_to_top(int *stack, int num, int ch) {
    stack_remove(stack, num, ch);
    stack[num - 1] = ch;
}

static void stack_release(int *stack, int num,
                          pg_mpe_channel_t *channels, int ch) {
    stack_remove(stack, num, ch);

    int insert_pos = num - 1;
    for (int i = 0; i < num - 1; i++) {
        int stack_ch = stack[i];
        int ch_idx = stack_ch - PG_MPE_FIRST_CH;
        if (channels[ch_idx].is_playing) {
            insert_pos = i;
            break;
        }
    }

    for (int i = num - 1; i > insert_pos; i--)
        stack[i] = stack[i - 1];
    stack[insert_pos] = ch;
}

/* ── MIDI helpers ────────────────────────────────────────────────── */

static void emit_pitch_bend(uint8_t out_msg[3], int ch_midi, int bend14) {
    out_msg[0] = 0xE0 | ((ch_midi - 1) & 0x0F);
    out_msg[1] = bend14 & 0x7F;
    out_msg[2] = (bend14 >> 7) & 0x7F;
}

static void emit_note_on(uint8_t out_msg[3], int ch_midi,
                          uint8_t note, uint8_t velocity) {
    out_msg[0] = 0x90 | ((ch_midi - 1) & 0x0F);
    out_msg[1] = note;
    out_msg[2] = velocity;
}

static void emit_note_off(uint8_t out_msg[3], int ch_midi,
                           uint8_t note, uint8_t velocity) {
    out_msg[0] = 0x80 | ((ch_midi - 1) & 0x0F);
    out_msg[1] = note;
    out_msg[2] = velocity;
}

static int find_channel_for_note(pg_mpe_state_t *state, uint8_t in_note) {
    for (int i = 0; i < PG_MPE_CHANNELS; i++) {
        if (state->channels[i].is_playing &&
            state->channels[i].in_note == in_note)
            return PG_MPE_FIRST_CH + i;
    }
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

void pg_mpe_init(pg_mpe_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->mpe_bend_range = 48;
    for (int i = 0; i < PG_MPE_CHANNELS; i++) {
        state->channel_stack[i] = PG_MPE_FIRST_CH + i;
        state->channels[i].is_playing = 0;
    }
}

int pg_mpe_process_midi(pg_mpe_state_t *state,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out) {
    if (!state || in_len < 1) return 0;

    uint8_t status = in_msg[0] & 0xF0;
    uint8_t note = (in_len >= 2) ? in_msg[1] : 0;
    uint8_t velocity = (in_len >= 3) ? in_msg[2] : 0;
    int out_count = 0;

    /* ── Note On ── */
    if (status == 0x90 && velocity > 0) {
        /* If already playing, release old channel */
        int existing_ch = find_channel_for_note(state, note);
        if (existing_ch >= 0) {
            int ch_idx = existing_ch - PG_MPE_FIRST_CH;
            if (out_count < max_out) {
                emit_note_off(out_msgs[out_count], existing_ch,
                              state->channels[ch_idx].out_note, 0);
                out_lens[out_count] = 3;
                out_count++;
            }
            state->channels[ch_idx].is_playing = 0;
            stack_release(state->channel_stack, PG_MPE_CHANNELS,
                          state->channels, existing_ch);
        }

        /* Allocate from bottom of stack (LRU) */
        int alloc_ch = state->channel_stack[0];
        int alloc_idx = alloc_ch - PG_MPE_FIRST_CH;

        /* Voice-steal if allocated channel still playing */
        if (state->channels[alloc_idx].is_playing) {
            if (out_count < max_out) {
                emit_note_off(out_msgs[out_count], alloc_ch,
                              state->channels[alloc_idx].out_note, 0);
                out_lens[out_count] = 3;
                out_count++;
            }
            state->channels[alloc_idx].is_playing = 0;
        }

        /* Calculate MPE output */
        double target = pg_log2freqs[note & 0x7F];
        uint8_t out_note;
        int out_bend14;
        calc_mpe_output(target, state->mpe_bend_range, &out_note, &out_bend14);

        /* Store channel state */
        state->channels[alloc_idx].in_note = note;
        state->channels[alloc_idx].out_note = out_note;
        state->channels[alloc_idx].out_bend14 = out_bend14;
        state->channels[alloc_idx].is_playing = 1;

        stack_move_to_top(state->channel_stack, PG_MPE_CHANNELS, alloc_ch);

        /* Emit: pitch bend first, then note-on */
        if (out_count < max_out) {
            emit_pitch_bend(out_msgs[out_count], alloc_ch, out_bend14);
            out_lens[out_count] = 3;
            out_count++;
        }
        if (out_count < max_out) {
            emit_note_on(out_msgs[out_count], alloc_ch, out_note, velocity);
            out_lens[out_count] = 3;
            out_count++;
        }
        return out_count;
    }

    /* ── Note Off ── */
    if (status == 0x80 || (status == 0x90 && velocity == 0)) {
        int ch = find_channel_for_note(state, note);
        if (ch < 0) {
            /* Not tracked — pass through */
            if (max_out < 1) return 0;
            int copy_len = in_len < 3 ? in_len : 3;
            memcpy(out_msgs[0], in_msg, copy_len);
            out_lens[0] = copy_len;
            return 1;
        }

        int ch_idx = ch - PG_MPE_FIRST_CH;
        state->channels[ch_idx].is_playing = 0;
        stack_release(state->channel_stack, PG_MPE_CHANNELS,
                      state->channels, ch);

        if (max_out < 1) return 0;
        emit_note_off(out_msgs[0], ch,
                      state->channels[ch_idx].out_note, velocity);
        out_lens[0] = 3;
        return 1;
    }

    /* ── Everything else: pass through ── */
    if (max_out < 1) return 0;
    int copy_len = in_len < 3 ? in_len : 3;
    memcpy(out_msgs[0], in_msg, copy_len);
    out_lens[0] = copy_len;
    return 1;
}

int pg_mpe_tick(pg_mpe_state_t *state,
                uint8_t out_msgs[][3], int out_lens[], int max_out) {
    if (!state || !state->retune_pending) return 0;

    state->retune_pending = 0;
    int out_count = 0;

    for (int i = 0; i < PG_MPE_CHANNELS && out_count < max_out; i++) {
        if (!state->channels[i].is_playing) continue;

        int ch_midi = PG_MPE_FIRST_CH + i;
        emit_pitch_bend(out_msgs[out_count], ch_midi,
                        state->channels[i].out_bend14);
        out_lens[out_count] = 3;
        out_count++;
    }
    return out_count;
}

void pg_mpe_retune(pg_mpe_state_t *state) {
    if (!state) return;
    for (int i = 0; i < PG_MPE_CHANNELS; i++)
        recalc_channel_bend(state, i);
    state->retune_pending = 1;
}

int pg_mpe_set_param(pg_mpe_state_t *state, const char *key, const char *val) {
    if (!state || !key || !val) return 0;

    if (strcmp(key, "mpe_bend_range") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= 96) state->mpe_bend_range = v;
        return 1;
    }
    return 0;
}

int pg_mpe_get_param(pg_mpe_state_t *state, const char *key,
                     char *buf, int buf_len) {
    if (!state || !key || !buf) return 0;

    if (strcmp(key, "mpe_bend_range") == 0) {
        snprintf(buf, buf_len, "%d", state->mpe_bend_range);
        return 1;
    }
    return 0;
}

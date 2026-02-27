/*
 * PitchGrid MPE — Channel Allocation + Pitch Bend
 *
 * Distributes incoming MIDI notes across MPE member channels 2-16
 * using an LRU channel stack. Looks up target frequency from the
 * scale module's log2freq table, then computes the closest standard
 * MIDI note + precise pitch bend.
 */

#ifndef PITCHGRID_MPE_H
#define PITCHGRID_MPE_H

#include <stdint.h>

#define PG_MPE_CHANNELS  15   /* Member channels 2-16 */
#define PG_MPE_FIRST_CH   2
#define PG_MPE_LAST_CH   16

/* log2(C4) — used for MIDI note ↔ log2freq conversion */
#define PG_LOG2_C4 8.031359713524660

/* ── Per-channel state ─────────────────────────────────────────── */

typedef struct {
    uint8_t in_note;      /* Original input MIDI note */
    uint8_t out_note;     /* Closest standard MIDI note sent to synth */
    int     out_bend14;   /* 14-bit pitch bend (0-16383, center=8192) */
    int     is_playing;   /* 1 if channel has an active voice */
} pg_mpe_channel_t;

/* ── MPE instance state ────────────────────────────────────────── */

typedef struct {
    pg_mpe_channel_t channels[PG_MPE_CHANNELS];
    int channel_stack[PG_MPE_CHANNELS];  /* LRU: [0]=next alloc, [14]=MRU */
    int mpe_bend_range;                  /* semitones (1-96) */
    int retune_pending;                  /* 1 = tick should emit bends */
} pg_mpe_state_t;

/* ── Functions ─────────────────────────────────────────────────── */

/* Initialize MPE state: channel stack [2..16], bend_range=48 */
void pg_mpe_init(pg_mpe_state_t *state);

/* Process a MIDI message: note on/off → MPE conversion.
 * Returns number of output messages written. */
int pg_mpe_process_midi(pg_mpe_state_t *state,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

/* Emit retuning pitch bends for playing channels.
 * Returns number of output messages written. */
int pg_mpe_tick(pg_mpe_state_t *state,
                uint8_t out_msgs[][3], int out_lens[], int max_out);

/* Recalculate bends for all playing channels after scale change. */
void pg_mpe_retune(pg_mpe_state_t *state);

/* Route set_param/get_param for MPE keys.
 * Returns 1 if key was handled, 0 otherwise. */
int pg_mpe_set_param(pg_mpe_state_t *state, const char *key, const char *val);
int pg_mpe_get_param(pg_mpe_state_t *state, const char *key,
                     char *buf, int buf_len);

#endif /* PITCHGRID_MPE_H */

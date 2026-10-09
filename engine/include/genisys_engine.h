#ifndef GENISYS_ENGINE_H
#define GENISYS_ENGINE_H

/* Genisys engine: turns MIDI-style notes and a patch into stereo audio at
 * any sample rate, using the synth-core chip emulations underneath.
 *
 * This is the layer every front-end shares (the plugin, the desktop app,
 * the test harness), so voice allocation, note-to-register conversion, patch
 * application and resampling live in exactly one place.
 *
 * Unlike synth-core, the engine targets desktop/plugin hosts and uses float
 * for its output stage. The chips themselves still run in integer math.
 *
 * Threading: an engine instance is NOT thread-safe. Call every function for
 * a given instance from one thread (normally the audio thread); front-ends
 * hand UI and MIDI changes over to that thread themselves. Call
 * genisys_engine_global_init() once, from a single thread, before creating
 * any engine. */

#include <stdint.h>

#include "psg.h"
#include "ym2612.h"
#include "genisys_resampler.h"

#define GENISYS_NUM_VOICES 6 /* one per YM2612 channel: the hardware limit */

/* ---- Patch: every field is a real register value, in register units ---- */

typedef struct {
    int mul;        /* 0-15 frequency multiplier (0 = x0.5) */
    int dt;         /* 0-7 detune (0 and 4 = none, 1-3 up, 5-7 down) */
    int tl;         /* 0-127 total level, 0 = loudest (0.75 dB steps) */
    int ar;         /* 0-31 attack rate */
    int d1r;        /* 0-31 first decay rate */
    int d2r;        /* 0-31 second decay rate, while sustaining */
    int sl;         /* 0-15 sustain level (3 dB steps; 15 = ~93 dB) */
    int rr;         /* 0-15 release rate */
    int ks;         /* 0-3 key scaling: higher notes get faster envelopes */
    int am;         /* 0/1 operator responds to LFO amplitude modulation */
    int ssg_enable; /* 0/1 SSG-EG looping envelope */
    int ssg_mode;   /* 0-7 SSG-EG shape */
} GenisysOperatorParams;

typedef struct {
    int algorithm;     /* 0-7 */
    int feedback;      /* 0-7 operator-1 self-feedback */
    int lfo_enable;    /* 0/1 (the LFO is chip-wide) */
    int lfo_rate;      /* 0-7, ~3.98 Hz to ~72.2 Hz */
    int ams;           /* 0-3 tremolo depth */
    int pms;           /* 0-7 vibrato depth */
    int velocity_sens; /* 0-100 %: 0 = every note equally loud (hardware-authentic) */
    GenisysOperatorParams op[4]; /* OP1..OP4 */
} GenisysPatch;

/* PSG layer (temporary shape, carried over from the desktop app: the
 * PSG's 3 tone channels double FM voices 0-2; a proper PSG instrument
 * arrives in a later phase). */
typedef struct {
    int level;        /* 0-15, 0 = off */
    int noise_on;     /* 0/1 */
    int noise_white;  /* 0 = periodic, 1 = white */
    int noise_rate;   /* 0-3 */
    int noise_volume; /* 0-15 */
} GenisysPsgSettings;

typedef enum {
    GENISYS_VOICE_FREE = 0,  /* never used, or released and fully silent */
    GENISYS_VOICE_HELD,      /* key down */
    GENISYS_VOICE_RELEASED   /* key up, release tail may still be sounding */
} GenisysVoiceState;

typedef struct {
    GenisysVoiceState state;
    int note;      /* MIDI note 0-127 */
    int velocity;  /* 1-127 */
    uint32_t age;  /* engine event counter at the last note-on/off, for oldest-first choices */
} GenisysVoice;

typedef struct {
    Ym2612Chip chip;
    Psg psg;
    GenisysPatch patch;
    GenisysPsgSettings psg_settings;
    GenisysVoice voice[GENISYS_NUM_VOICES];
    uint32_t event_counter;

    double psg_ticks_per_fm_sample;
    double psg_tick_accum;

    GenisysResampler resampler;
    double sample_rate;
} GenisysEngine;

/* Builds the chip lookup tables. Call once, from one thread, before
 * creating any engine. */
void genisys_engine_global_init(void);

void genisys_engine_init(GenisysEngine *e, double sample_rate);

/* Changing the rate restarts the resampler (a few samples of silence). */
void genisys_engine_set_sample_rate(GenisysEngine *e, double sample_rate);

/* Applies to all voices immediately, including notes already sounding. */
void genisys_engine_set_patch(GenisysEngine *e, const GenisysPatch *patch);
void genisys_engine_set_psg(GenisysEngine *e, const GenisysPsgSettings *settings);

/* note: MIDI note number (60 = middle C). velocity: 1-127. */
void genisys_engine_note_on(GenisysEngine *e, int note, int velocity);
void genisys_engine_note_off(GenisysEngine *e, int note);
void genisys_engine_all_notes_off(GenisysEngine *e);

/* Renders `frames` stereo samples. Output is nominally -1..1; the chip's
 * full mix can exceed that (6 loud voices reach about +/-1.5) instead of
 * hard-clipping, so the host or a master gain decides how to handle it. */
void genisys_engine_render(GenisysEngine *e, float *out_left, float *out_right, int frames);

/* ---- Helpers, exposed for front-ends and tests ---- */

GenisysPatch genisys_default_patch(void);

/* Converts a MIDI note to the chip's (block, fnum) frequency registers. */
void genisys_note_to_block_fnum(int note, int *block, int *fnum);

/* Which operators reach the output (carriers) for an algorithm, as bits:
 * bit0 = OP1 .. bit3 = OP4. */
uint8_t genisys_carrier_mask(int algorithm);

#endif /* GENISYS_ENGINE_H */

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

#ifdef __cplusplus
extern "C" {
#endif

#define GENISYS_NUM_VOICES 6 /* one per YM2612 channel: the hardware limit */

typedef enum {
    GENISYS_MODE_POLY = 0,   /* up to 6 notes (fewer with unison or drums) */
    GENISYS_MODE_MONO = 1,   /* one note; each new note restarts the envelope */
    GENISYS_MODE_LEGATO = 2  /* one note; overlapping notes only change pitch */
} GenisysVoiceMode;

/* ---- Patch: register values, plus the engine's own performance settings ---- */

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
    int vibrato_depth; /* 0-100 cents at full mod wheel (software vibrato, as game drivers did) */
    int vibrato_rate;  /* tenths of a Hz, 10-150 (1.0-15.0 Hz) */
    int voice_mode;    /* GenisysVoiceMode */
    int glide_time;    /* ms, 0-2000: portamento in Mono/Legato (0 = off) */
    int unison;        /* 1-3 chip channels per note */
    int unison_detune; /* cents, 0-50: spread between unison channels */
    int unison_stereo; /* 0/1: pan unison channels hard left/right (real chip panning) */

    /* "Quick Sound" macros: relative tweaks on top of the operator settings,
     * -100..+100, 0 = as programmed. They let anyone reshape a preset
     * without knowing which operators do what. */
    int macro_bright;  /* + = brighter: lowers every modulator's TL (carriers untouched) */
    int macro_attack;  /* + = slower attack on every operator */
    int macro_decay;   /* + = longer decays (operators that already sustain keep sustaining) */
    int macro_release; /* + = longer release */
    int vibrato_amount; /* 0-100 %: vibrato without the mod wheel (the wheel can add more) */
    GenisysOperatorParams op[4]; /* OP1..OP4 */
} GenisysPatch;

/* PSG layer: the SN76489's 3 square-wave channels played alongside FM.
 * Envelopes step once per 60 Hz video frame, as game sound drivers did,
 * which gives the PSG its characteristic stepped fades. */
typedef enum {
    GENISYS_PSG_OFF = 0,
    GENISYS_PSG_UNISON = 1,   /* the 3 PSG channels double the most recent FM notes */
    GENISYS_PSG_ARPEGGIO = 2  /* one PSG channel cycles through all held notes */
} GenisysPsgMode;

typedef struct {
    int mode;         /* GenisysPsgMode */
    int level;        /* 0-15 */
    int octave;       /* -2..+2, relative to the FM note */
    int attack;       /* 0-15: frames per volume step while rising (0 = instant) */
    int decay;        /* 0-15: frames per step while falling to sustain (0 = instant) */
    int sustain;      /* 0-15: held volume (15 = full) */
    int release;      /* 0-15: frames per step after key-up (0 = instant) */
    int arp_speed;    /* 1-8: frames per arpeggio step */

    /* Continuous noise "drone" on the noise channel (drum hits override it). */
    int noise_on;     /* 0/1 */
    int noise_white;  /* 0 = periodic, 1 = white */
    int noise_rate;   /* 0-3 */
    int noise_volume; /* 0-15 */
} GenisysPsgSettings;

/* Drum kit (see genisys_drums.h). While enabled, FM channel 6 belongs to the
 * DAC, so FM has 5 voices -- the same trade-off Genesis games made. */
typedef struct {
    int enabled; /* 0/1 */
    int level;   /* 0-100 % */
} GenisysDrumSettings;

#define GENISYS_PSG_TONE_CHANNELS 3

typedef enum {
    GENISYS_ENV_OFF = 0,
    GENISYS_ENV_ATTACK,
    GENISYS_ENV_DECAY,
    GENISYS_ENV_SUSTAIN,
    GENISYS_ENV_RELEASE
} GenisysEnvStage;

typedef struct {
    int note;            /* MIDI note, -1 = none */
    int gate;            /* 1 while the key is held */
    GenisysEnvStage stage;
    int volume;          /* 0-15 envelope output */
    int counter;         /* frames until the next envelope step */
    uint32_t age;
} GenisysPsgVoice;

/* The console's sound path after the chip. */
typedef enum {
    GENISYS_CHIP_YM2612 = 0, /* Model 1: 9-bit DAC with the gritty "ladder effect" */
    GENISYS_CHIP_YM3438 = 1, /* Model 2 and later: 9-bit DAC, ladder effect fixed */
    GENISYS_CHIP_CLEAN = 2   /* no DAC artifacts: full 14-bit channel output */
} GenisysChipModel;

typedef struct {
    int chip_model;   /* GenisysChipModel */
    int filter_on;    /* 0/1: console output low-pass filter */
    double filter_hz; /* cutoff of the first-order (6 dB/octave) low-pass */
} GenisysConsoleSettings;

/* Default cutoff: ~3.68 kHz, first order. No hardware measurement of the
 * Model 1 filter has been published; this is the corner the Mega Amp
 * replacement board uses to imitate the Model 1, which matches reports
 * that the Model 1 filter is first order. Adjustable for that reason. */
#define GENISYS_MODEL1_FILTER_HZ 3680.0

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
    int sustained; /* key is up but the sustain pedal is holding the note */
    double detune; /* semitones: this voice's unison offset */
    uint8_t pan;   /* $B4 pan bits: 0xC0 both, 0x80 left, 0x40 right */
    int last_block, last_fnum; /* frequency registers last written, to skip redundant writes */
} GenisysVoice;

typedef struct {
    Ym2612Chip chip;
    Psg psg;
    GenisysPatch patch;
    GenisysPsgSettings psg_settings;
    GenisysDrumSettings drum_settings;
    GenisysVoice voice[GENISYS_NUM_VOICES];
    uint32_t event_counter;

    double psg_ticks_per_fm_sample;
    double psg_tick_accum;

    /* 60 Hz "frame" clock for PSG envelopes, arpeggios and noise drums. */
    double frame_accum;

    GenisysPsgVoice psg_voice[GENISYS_PSG_TONE_CHANNELS];
    int arp_index;
    int arp_counter;
    int psg_pitch_dirty;

    int noise_drum_active;
    int noise_drum_volume_q;   /* in quarter volume steps */
    int noise_drum_decay_q;

    /* DAC drum playback (one sample at a time, like the real DAC channel). */
    const int8_t *dac_sample;
    int dac_length;
    double dac_pos;
    float dac_gain;
    int dac_last_written;

    /* Output stage, run at the chip's native rate before resampling. */
    GenisysConsoleSettings console;
    float dc_coeff;            /* DC blocker pole (~5 Hz): the console's output capacitors */
    float dc_x_l, dc_x_r, dc_y_l, dc_y_r;
    int dc_primed;

    /* Expression: pitch bend, mod-wheel vibrato, sustain pedal. Pitch is
     * re-evaluated at control rate (every ENGINE_CONTROL_PERIOD native
     * samples, ~1.7 kHz), which keeps bends and vibrato smooth. */
    double bend_semitones;
    double mod_wheel;      /* 0-1 */
    double vibrato_phase;  /* 0-1 */
    int sustain_pedal;
    int control_counter;

    /* Mono/Legato: held keys (last-note priority) and the gliding pitch. */
    int note_stack[16];
    int stack_count;
    double mono_pitch, mono_target, glide_step;
    int mono_has_pitch;
    int mono_velocity;
    float lp_coeff;            /* low-pass pole for console.filter_hz */
    float lp_l, lp_r;

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
void genisys_engine_set_console(GenisysEngine *e, const GenisysConsoleSettings *settings);
void genisys_engine_set_drums(GenisysEngine *e, const GenisysDrumSettings *settings);

/* Plays a drum from the kit (General MIDI drum note numbers, e.g. 36 kick,
 * 38 snare, 42 closed hat). Ignored while drums are disabled or for notes
 * the kit doesn't map. Front-ends route MIDI channel 10 here. */
void genisys_engine_drum_hit(GenisysEngine *e, int note, int velocity);

/* note: MIDI note number (60 = middle C). velocity: 1-127. */
void genisys_engine_note_on(GenisysEngine *e, int note, int velocity);
void genisys_engine_note_off(GenisysEngine *e, int note);
void genisys_engine_all_notes_off(GenisysEngine *e);

/* Pitch bend in semitones (front-ends map the MIDI wheel through their own
 * bend range). Applies to every sounding note, FM and PSG. */
void genisys_engine_pitch_bend(GenisysEngine *e, double semitones);

/* Mod wheel, 0-1: scales the patch's vibrato depth. */
void genisys_engine_mod_wheel(GenisysEngine *e, double amount);

/* Sustain pedal: while down, key-ups are held until the pedal is lifted. */
void genisys_engine_sustain(GenisysEngine *e, int down);

/* Renders `frames` stereo samples. Output is nominally -1..1; the chip's
 * full mix can exceed that (6 loud voices reach about +/-1.5) instead of
 * hard-clipping, so the host or a master gain decides how to handle it. */
void genisys_engine_render(GenisysEngine *e, float *out_left, float *out_right, int frames);

/* ---- Helpers, exposed for front-ends and tests ---- */

GenisysPatch genisys_default_patch(void);

GenisysPsgSettings genisys_default_psg(void);

/* YM2612 (Model 1) with the console filter on: the classic sound. */
GenisysConsoleSettings genisys_default_console(void);

/* Converts a MIDI note to the chip's (block, fnum) frequency registers. */
void genisys_note_to_block_fnum(int note, int *block, int *fnum);

/* Same for a fractional pitch in semitones (69.0 = A4 = 440 Hz). */
void genisys_pitch_to_block_fnum(double pitch, int *block, int *fnum);

/* Which operators reach the output (carriers) for an algorithm, as bits:
 * bit0 = OP1 .. bit3 = OP4. */
uint8_t genisys_carrier_mask(int algorithm);

/* Which of the 6 chip channels are making sound right now (bit n = channel
 * n): held notes and release tails. For the UI's voice lights. */
uint8_t genisys_engine_active_voices(const GenisysEngine *e);

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_ENGINE_H */

#ifndef GENISYS_DRUMS_H
#define GENISYS_DRUMS_H

/* The built-in drum kit, mapped to General MIDI drum notes.
 *
 * Like most Genesis soundtracks, the kit splits across both chips:
 *  - kicks, snares, claps, rim and toms are 8-bit samples played through
 *    the YM2612's DAC (channel 6), one at a time, as game sound drivers did;
 *  - hi-hats and cymbals use the PSG's noise channel with a volume envelope
 *    that steps once per 60 Hz video frame, the way game drivers updated it.
 *
 * The samples are synthesized from scratch at startup (no recordings are
 * shipped), using a fixed pseudo-random seed so every platform builds the
 * same bytes. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample rate of the DAC samples. Game drivers played DAC samples at
 * roughly 8-20 kHz; this sits in that range, which is part of the sound. */
#define GENISYS_DRUM_SAMPLE_HZ 13000.0

typedef enum {
    GENISYS_DRUM_NONE = 0,
    GENISYS_DRUM_DAC,   /* 8-bit sample on the YM2612 DAC */
    GENISYS_DRUM_NOISE  /* PSG noise channel with a stepped envelope */
} GenisysDrumKind;

typedef struct {
    GenisysDrumKind kind;
    const char *name;

    /* GENISYS_DRUM_DAC */
    const int8_t *sample;
    int length;

    /* GENISYS_DRUM_NOISE */
    int noise_white;      /* 1 = white, 0 = periodic */
    int noise_rate;       /* 0-2: high, medium, low */
    int start_volume;     /* 1-15 at full velocity */
    int decay_per_frame;  /* volume steps lost per frame, in 1/4 steps (4 = one step) */
} GenisysDrum;

/* Generates the samples. Called by genisys_engine_global_init(). */
void genisys_drums_init(void);

/* The drum for a MIDI note, or one with kind GENISYS_DRUM_NONE. */
const GenisysDrum *genisys_drum_for_note(int note);

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_DRUMS_H */

#ifndef GENISYS_ENGINE_INTERNAL_H
#define GENISYS_ENGINE_INTERNAL_H

/* Shared between the engine's own source files; not part of the public API. */

#include "genisys_drums.h"
#include "genisys_engine.h"

/* Native-rate samples per 60 Hz frame (NTSC video timing, which game sound
 * drivers ran on). */
#define ENGINE_SAMPLES_PER_FRAME (YM2612_SAMPLE_HZ / 60.0)

/* Native samples between pitch updates (bend, vibrato): ~1.7 kHz. */
#define ENGINE_CONTROL_PERIOD 32

/* Semitones to add to every note right now: bend + vibrato. */
double engine_pitch_offset(const GenisysEngine *e);

/* Linear gain for a velocity, using the engine's velocity curve at 100%. */
float engine_velocity_gain(int velocity);

/* The same curve as attenuation in dB (0 at velocity 127). */
float engine_velocity_db(int velocity);

/* PSG volume is logarithmic: each of its 16 steps is 2 dB. Converts an
 * attenuation in dB to whole PSG steps. */
int engine_db_to_psg_steps(float db);

/* ---- PSG layer (genisys_psg.c) ---- */
void engine_psg_reset(GenisysEngine *e);
void engine_psg_apply_settings(GenisysEngine *e, const GenisysPsgSettings *previous);
void engine_psg_note_on(GenisysEngine *e, int note);
void engine_psg_note_off(GenisysEngine *e, int note);
void engine_psg_all_notes_off(GenisysEngine *e);
void engine_psg_frame(GenisysEngine *e);
void engine_psg_update_pitch(GenisysEngine *e);
void engine_psg_noise_drum(GenisysEngine *e, const GenisysDrum *drum, int velocity);

#endif /* GENISYS_ENGINE_INTERNAL_H */

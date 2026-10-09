#ifndef GENISYS_ENGINE_INTERNAL_H
#define GENISYS_ENGINE_INTERNAL_H

/* Shared between the engine's own source files; not part of the public API. */

#include "genisys_drums.h"
#include "genisys_engine.h"

/* Native-rate samples per 60 Hz frame (NTSC video timing, which game sound
 * drivers ran on). */
#define ENGINE_SAMPLES_PER_FRAME (YM2612_SAMPLE_HZ / 60.0)

/* Linear gain for a velocity, using the engine's velocity curve at 100%. */
float engine_velocity_gain(int velocity);

/* ---- PSG layer (genisys_psg.c) ---- */
void engine_psg_reset(GenisysEngine *e);
void engine_psg_apply_settings(GenisysEngine *e, const GenisysPsgSettings *previous);
void engine_psg_note_on(GenisysEngine *e, int note);
void engine_psg_note_off(GenisysEngine *e, int note);
void engine_psg_all_notes_off(GenisysEngine *e);
void engine_psg_frame(GenisysEngine *e);
void engine_psg_noise_drum(GenisysEngine *e, const GenisysDrum *drum, float gain);

#endif /* GENISYS_ENGINE_INTERNAL_H */

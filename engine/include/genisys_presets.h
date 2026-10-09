#ifndef GENISYS_PRESETS_H
#define GENISYS_PRESETS_H

/* The factory preset library, shared by every front-end (the plugin
 * exposes it as its program list). Every preset is original: designed from
 * FM principles, not copied from game soundtracks. */

#include "genisys_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

int genisys_preset_count(void);
const char *genisys_preset_name(int index);     /* NULL if out of range */
const char *genisys_preset_category(int index); /* e.g. "Bass", "Keys"; NULL if out of range */

/* The sound and its performance settings (voice mode, glide, unison...). */
GenisysPatch genisys_preset_patch(int index);   /* default patch if out of range */

/* The preset's PSG layer (Off for most presets). */
GenisysPsgSettings genisys_preset_psg(int index);

/* Effects belong to the plugin, not the engine; presets still suggest them.
 * A mix of 0 means that effect is off. echo_division indexes the plugin's
 * synced-echo list: 1/4, 1/8, 1/8 dotted, 1/16, 1/4 dotted, 1/8 triplet. */
typedef struct {
    int chorus_mix;    /* 0-100 */
    int echo_mix;      /* 0-100 */
    int echo_feedback; /* 0-95 */
    int echo_division; /* 0-5 */
    int reverb_mix;    /* 0-100 */
    int reverb_size;   /* 0-100 */
} GenisysPresetFx;

GenisysPresetFx genisys_preset_fx(int index);

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_PRESETS_H */

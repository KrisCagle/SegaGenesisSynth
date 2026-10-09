#ifndef GENISYS_PRESETS_H
#define GENISYS_PRESETS_H

/* Factory presets, shared by every front-end (the plugin exposes them as
 * its program list). Phase 6 of the roadmap grows this into a categorized
 * library. */

#include "genisys_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

int genisys_preset_count(void);
const char *genisys_preset_name(int index);   /* NULL if out of range */
GenisysPatch genisys_preset_patch(int index); /* default patch if out of range */

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_PRESETS_H */

#include "genisys_presets.h"

#include <stddef.h>

/* Compact preset description: register values only, everything not listed
 * (D2R, KS, AM, SSG-EG, LFO) is off. */
typedef struct {
    int mul, dt, tl, ar, d1r, sl, rr;
} OpDef;

typedef struct {
    const char *name;
    int algorithm, feedback;
    OpDef op[4]; /* OP1..OP4 */
} PresetDef;

/* A handful of starting points -- FM's parameter space is huge, so these
 * are hand-picked to land somewhere recognizable; tweak from here by ear. */
static const PresetDef PRESETS[] = {
    { "Init", 0, 0, { { 1, 4, 22, 31, 10, 4, 8 }, { 1, 4, 26, 31, 10, 4, 8 },
                      { 1, 4, 30, 31, 10, 4, 8 }, { 1, 4, 5, 31, 10, 4, 8 } } },
    { "E.Piano", 4, 0, { { 1, 4, 8, 31, 8, 3, 7 }, { 1, 4, 2, 27, 6, 3, 6 },
                         { 2, 4, 16, 31, 12, 3, 8 }, { 1, 4, 10, 27, 8, 3, 7 } } },
    { "Bass", 0, 3, { { 1, 4, 28, 31, 14, 6, 10 }, { 2, 4, 32, 31, 14, 6, 10 },
                      { 1, 4, 20, 31, 10, 4, 9 }, { 1, 4, 4, 31, 8, 2, 9 } } },
    { "Bell", 5, 0, { { 1, 7, 8, 31, 6, 2, 6 }, { 1, 4, 6, 31, 4, 1, 5 },
                      { 2, 4, 14, 31, 6, 2, 6 }, { 3, 4, 20, 31, 8, 3, 7 } } },
    { "Brass", 4, 2, { { 1, 4, 18, 25, 10, 4, 9 }, { 1, 4, 6, 22, 8, 3, 8 },
                       { 1, 4, 22, 25, 10, 4, 9 }, { 1, 4, 8, 22, 8, 3, 8 } } },
    { "Lead", 2, 4, { { 3, 4, 26, 31, 12, 5, 9 }, { 1, 4, 20, 31, 10, 4, 8 },
                      { 1, 4, 14, 31, 10, 4, 8 }, { 1, 4, 6, 31, 8, 3, 8 } } },
};

#define PRESET_COUNT ((int)(sizeof PRESETS / sizeof PRESETS[0]))

int genisys_preset_count(void) {
    return PRESET_COUNT;
}

const char *genisys_preset_name(int index) {
    return (index >= 0 && index < PRESET_COUNT) ? PRESETS[index].name : NULL;
}

GenisysPatch genisys_preset_patch(int index) {
    GenisysPatch p = genisys_default_patch();
    const PresetDef *d;
    int i;

    if (index < 0 || index >= PRESET_COUNT) return p;
    d = &PRESETS[index];
    p.algorithm = d->algorithm;
    p.feedback = d->feedback;
    for (i = 0; i < 4; i++) {
        GenisysOperatorParams *o = &p.op[i];
        const OpDef *src = &d->op[i];
        o->mul = src->mul; o->dt = src->dt; o->tl = src->tl; o->ar = src->ar;
        o->d1r = src->d1r; o->sl = src->sl; o->rr = src->rr;
        o->d2r = 0; o->ks = 0; o->am = 0; o->ssg_enable = 0; o->ssg_mode = 0;
    }
    return p;
}

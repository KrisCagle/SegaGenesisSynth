#ifndef GENISYS_PRESET_LOUDNESS_H
#define GENISYS_PRESET_LOUDNESS_H

/* Shared by tools/preset_levels.c (which suggests each preset's `level`)
 * and tests/engine_tests.c (which checks presets stay balanced).
 *
 * Loudness = the loudest 50 ms stretch (RMS, dBFS) of one note held for
 * 0.8 s at velocity 100, played in the preset's natural range. Short
 * windows let plucks and pads be compared fairly: a pluck is judged by its
 * attack, not by its decay. */

#include <math.h>

#include "genisys_engine.h"
#include "genisys_presets.h"

#define PRESET_LOUDNESS_TARGET_DB (-20.0)

static int preset_loudness_note(int index) {
    const char *category = genisys_preset_category(index);
    if (category == NULL) return 60;
    if (category[0] == 'B' && category[1] == 'a') return 40;  /* Bass: E2 */
    if (category[0] == 'C' && category[1] == 'h') return 72;  /* Chip: C5 */
    if (category[0] == 'S' && category[1] == 'F') return 60;
    return 60;
}

static double preset_loudness_db(GenisysEngine *e, int index) {
    enum { RATE = 48000, FRAMES = RATE * 8 / 10, WINDOW = RATE / 20 };
    static float left[FRAMES], right[FRAMES];
    GenisysPatch patch = genisys_preset_patch(index);
    GenisysPsgSettings psg = genisys_preset_psg(index);
    double best = 0.0;
    int start;

    genisys_engine_init(e, RATE);
    genisys_engine_set_patch(e, &patch);
    genisys_engine_set_psg(e, &psg);
    genisys_engine_note_on(e, preset_loudness_note(index), 100);
    genisys_engine_render(e, left, right, FRAMES);

    for (start = 0; start + WINDOW <= FRAMES; start += WINDOW / 2) {
        double sum = 0.0;
        int i;
        for (i = start; i < start + WINDOW; i++) sum += 0.5 * ((double)left[i] * left[i] + (double)right[i] * right[i]);
        if (sum > best) best = sum;
    }
    best = sqrt(best / WINDOW);
    return best > 0.0 ? 20.0 * log10(best) : -120.0;
}

#endif /* GENISYS_PRESET_LOUDNESS_H */

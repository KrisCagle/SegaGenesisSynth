/* Measures every factory preset's loudness and suggests the `level` (a
 * carrier TL offset, 0.75 dB per step) that brings it to the target, so
 * switching presets doesn't jump in volume:
 *
 *   preset_levels
 *
 * Paste the suggested values into engine/src/genisys_presets.c. The levels
 * already in the table are included in the measurement, so a balanced
 * library prints "+0" everywhere. */

#include <stdio.h>

#include "preset_loudness.h"

int main(void) {
    static GenisysEngine engine;
    int i, worst = 0;

    genisys_engine_global_init();
    printf("%-18s %-22s %9s  %s\n", "category", "preset", "loudness", "suggested level change");
    for (i = 0; i < genisys_preset_count(); i++) {
        double db = preset_loudness_db(&engine, i);
        int change = (int)((db - PRESET_LOUDNESS_TARGET_DB) / 0.75 + (db > PRESET_LOUDNESS_TARGET_DB ? 0.5 : -0.5));
        printf("%-18s %-22s %6.1f dB  %+d\n", genisys_preset_category(i), genisys_preset_name(i), db, change);
        if (change > worst || -change > worst) worst = change > 0 ? change : -change;
    }
    printf("\nlargest change: %d steps (%.1f dB)\n", worst, worst * 0.75);
    return 0;
}

/* Finds factory presets that sound too much alike:
 *
 *   preset_similarity [pairs]
 *
 * Fingerprints every preset (see preset_fingerprint.h), then lists the
 * closest pairs, each preset's nearest neighbour, and an overall "variety"
 * score (the average nearest-neighbour distance; higher = a more varied
 * library). tests/engine_tests.c checks no pair gets too close. */

#include <stdio.h>
#include <stdlib.h>

#include "preset_fingerprint.h"

#define MAX_PRESETS 256

typedef struct { int a, b; double d; } Pair;

static int by_distance(const void *x, const void *y) {
    const double dx = ((const Pair *)x)->d, dy = ((const Pair *)y)->d;
    return (dx > dy) - (dx < dy);
}

int main(int argc, char **argv) {
    static GenisysEngine engine;
    static PresetFingerprint prints[MAX_PRESETS];
    static Pair pairs[MAX_PRESETS * MAX_PRESETS / 2];
    const int count = genisys_preset_count() < MAX_PRESETS ? genisys_preset_count() : MAX_PRESETS;
    const int show = argc > 1 ? atoi(argv[1]) : 25;
    int i, j, n = 0;
    double variety = 0.0;

    genisys_engine_global_init();
    for (i = 0; i < count; i++) preset_fingerprint(&engine, i, &prints[i]);

    for (i = 1; i < count; i++) /* preset 0 is Init */
        for (j = i + 1; j < count; j++) {
            pairs[n].a = i; pairs[n].b = j; pairs[n].d = preset_fingerprint_distance(&prints[i], &prints[j]);
            n++;
        }
    qsort(pairs, (size_t)n, sizeof pairs[0], by_distance);

    printf("Closest pairs (lower = more alike):\n");
    for (i = 0; i < show && i < n; i++)
        printf("  %5.2f  %-20s %-16s ~ %-20s %s\n", pairs[i].d, genisys_preset_name(pairs[i].a), genisys_preset_category(pairs[i].a),
               genisys_preset_name(pairs[i].b), genisys_preset_category(pairs[i].b));

    printf("\nNearest neighbour of each preset:\n");
    for (i = 1; i < count; i++) {
        int best = -1;
        double bestD = 1e9;
        for (j = 1; j < count; j++) {
            double d;
            if (j == i) continue;
            d = preset_fingerprint_distance(&prints[i], &prints[j]);
            if (d < bestD) { bestD = d; best = j; }
        }
        variety += bestD;
        printf("  %-16s %-20s %5.2f  %s\n", genisys_preset_category(i), genisys_preset_name(i), bestD, genisys_preset_name(best));
    }
    printf("\nVariety score (average nearest-neighbour distance): %.2f over %d presets\n", variety / (count - 1), count - 1);
    return 0;
}

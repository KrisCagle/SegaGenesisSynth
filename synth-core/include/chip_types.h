#ifndef CHIP_TYPES_H
#define CHIP_TYPES_H

#include <stdint.h>

/* Shared across all chip cores in synth-core/. This header must stay free of
 * any platform-specific includes (no stdio, no OS audio APIs) so the same
 * cores compile unmodified for both the PC prototype and, later, the GBA. */

typedef int16_t sample_t; /* mono sample, matches chip's native DAC width */

/* All math in synth-core is plain integer/fixed-point, never `float`/`double`
 * in the real-time signal path -- the GBA's ARM7TDMI has no FPU, and this
 * also mirrors how the real chips themselves work internally (e.g. the
 * YM2612's envelope/waveform tables are fixed-width integer lookups, not
 * floating-point). The one accepted exception is one-time startup table
 * generation (e.g. deriving a sine/log table from libm at init), which costs
 * nothing at runtime and isn't part of the per-sample hot path. */

static inline int16_t clamp_s16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

#endif /* CHIP_TYPES_H */

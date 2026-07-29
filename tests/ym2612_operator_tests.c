/* Numeric sanity checks for the YM2612 single-operator core (milestone 2a).
 * Not exhaustive -- covers the phase-increment formula, envelope monotonicity
 * through attack/release, and a structural sine/exp round-trip check. */

#include <stdio.h>
#include <stdlib.h>
#include "ym2612.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

/* fc_raw=(fnum<<block)>>1, dt=0 (FD=0 row is all zeros), mul=1 (MUL field 0
 * -> x0.5, matching the real chip's encoding) -> phase_inc = fc_raw >> 1. */
static void test_phase_increment_formula(void) {
    Ym2612Operator op;
    uint16_t fnum = 644;
    uint8_t block = 4;
    uint32_t expected_fc_raw = ((uint32_t)fnum << block) >> 1; /* 5152 */
    uint32_t expected_phase_inc = (expected_fc_raw * 1u) >> 1; /* 2576 */

    ym2612_operator_init(&op);
    ym2612_operator_set_dt_mul(&op, 0x00);
    ym2612_operator_set_freq(&op, fnum, block);

    CHECK(op.phase_inc == expected_phase_inc, "phase_inc matches (fnum<<block)>>1 formula (DT=0, MUL=0)");
}

/* Non-zero MUL should scale phase_inc linearly (MUL field N -> multiplier
 * N, or 0.5 when N==0), independent of DT. */
static void test_multiple_scales_phase_increment(void) {
    Ym2612Operator op;
    uint32_t inc_mul1, inc_mul4;

    ym2612_operator_init(&op);
    ym2612_operator_set_dt_mul(&op, 0x01); /* MUL field 1 -> x1 */
    ym2612_operator_set_freq(&op, 644, 4);
    inc_mul1 = op.phase_inc;

    ym2612_operator_set_dt_mul(&op, 0x04); /* MUL field 4 -> x4 */
    ym2612_operator_set_freq(&op, 644, 4);
    inc_mul4 = op.phase_inc;

    CHECK(inc_mul4 == inc_mul1 * 4, "MUL field scales phase_inc proportionally (x1 vs x4)");
}

/* A moderate (non-maxed) attack rate should move volume monotonically
 * toward 0 (loudest) every time it updates, and eventually leave the
 * attack state. Using AR=31 (max) would trigger the real chip's "attack
 * blocked / instant key-on" quirk instead, so we deliberately use a
 * slower rate here to exercise the gradual attack curve. */
static void test_attack_monotonically_increases_volume(void) {
    Ym2612Operator op;
    int i;
    int32_t last_volume;
    int saw_change = 0;
    int left_attack = 0;

    ym2612_operator_init(&op);
    ym2612_operator_set_freq(&op, 644, 0);   /* kc = 0 */
    ym2612_operator_set_dt_mul(&op, 0x00);
    ym2612_operator_set_ar_ksr(&op, 0x14);   /* AR=20, KSR=0 */
    ym2612_operator_set_d1r(&op, 0x00);
    ym2612_operator_set_d2r(&op, 0x00);
    ym2612_operator_set_sl_rr(&op, 0x20);    /* SL=2 (nonzero), RR=0 */
    ym2612_operator_set_tl(&op, 0);

    ym2612_operator_key_on(&op);
    CHECK(op.state == YM_EG_ATT, "key-on with moderate AR enters attack state");

    last_volume = op.volume;
    for (i = 0; i < 20000 && op.state == YM_EG_ATT; i++) {
        ym2612_operator_clock(&op);
        if (op.volume != last_volume) {
            saw_change = 1;
            if (op.volume > last_volume) {
                CHECK(0, "attack volume step should never increase attenuation (get quieter)");
            }
            last_volume = op.volume;
        }
    }
    if (op.state != YM_EG_ATT) left_attack = 1;

    CHECK(saw_change, "attack phase actually updates volume over time");
    CHECK(left_attack, "attack phase eventually transitions out of YM_EG_ATT");
}

/* After key-off from a loud sustain, volume should climb monotonically
 * (get quieter) until it saturates at MAX_ATT_INDEX and the operator turns off. */
static void test_release_monotonically_decreases_volume(void) {
    Ym2612Operator op;
    int i;
    int32_t last_volume;
    int reached_off = 0;

    ym2612_operator_init(&op);
    ym2612_operator_set_freq(&op, 644, 0);
    ym2612_operator_set_dt_mul(&op, 0x00);
    ym2612_operator_set_ar_ksr(&op, 0x1F);   /* max AR: instant key-on to volume=0 */
    ym2612_operator_set_d1r(&op, 0x00);
    ym2612_operator_set_d2r(&op, 0x00);      /* SL=0 below forces straight to sustain */
    ym2612_operator_set_sl_rr(&op, 0x08);    /* SL=0, RR=8 */
    ym2612_operator_set_tl(&op, 0);

    ym2612_operator_key_on(&op);
    CHECK(op.volume == 0 && op.state == YM_EG_SUS, "maxed AR + SL=0 jumps straight to loud sustain");

    ym2612_operator_key_off(&op);
    CHECK(op.state == YM_EG_REL, "key-off moves a sustaining operator into release");

    last_volume = op.volume;
    for (i = 0; i < 200000 && op.state != YM_EG_OFF; i++) {
        ym2612_operator_clock(&op);
        if (op.volume < last_volume) {
            CHECK(0, "release volume step should never decrease attenuation (get louder)");
        }
        last_volume = op.volume;
    }
    if (op.state == YM_EG_OFF) reached_off = 1;

    CHECK(reached_off, "release phase eventually reaches YM_EG_OFF");
    CHECK(op.volume >= 1023, "operator is fully attenuated (silent) once off");
}

/* Structural sine/exp round-trip check: with the envelope pinned at zero
 * attenuation (full volume, no TL) and a hand-picked phase_inc that lands
 * on exact quarter-table indices, the four samples of one cycle should
 * read as [big +peak, small, big -peak, small] -- i.e. recognizably a
 * sine, not noise or a flat line. */
static void test_sine_exp_round_trip_shape(void) {
    Ym2612Operator op;
    sample_t s[4];
    int i;

    ym2612_operator_init(&op);
    ym2612_operator_set_ar_ksr(&op, 0x1F); /* max AR -> instant volume=0 on key-on */
    ym2612_operator_set_d2r(&op, 0x00);    /* sustain rate 0 -> holds flat */
    ym2612_operator_set_sl_rr(&op, 0x00);  /* SL=0 -> straight to sustain */
    ym2612_operator_set_tl(&op, 0);
    ym2612_operator_key_on(&op);

    op.phase_inc = 262144u; /* SIN_LEN<<SIN_BITS / 4, i.e. exactly 1/4 turn per sample */

    for (i = 0; i < 4; i++) {
        s[i] = ym2612_operator_clock(&op);
    }

    CHECK(s[0] > 0 && s[2] < 0, "quarter-turn samples 0 and 2 land on opposite peaks");
    CHECK(iabs32(s[0]) > iabs32(s[1]) * 4 && iabs32(s[2]) > iabs32(s[3]) * 4,
          "peak samples are clearly larger in magnitude than the near-zero-crossing samples");
    CHECK(iabs32(iabs32(s[0]) - iabs32(s[2])) < iabs32(s[0]) / 10,
          "positive and negative peaks have roughly equal magnitude");
}

int main(void) {
    test_phase_increment_formula();
    test_multiple_scales_phase_increment();
    test_attack_monotonically_increases_volume();
    test_release_monotonically_decreases_volume();
    test_sine_exp_round_trip_shape();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d test(s) FAILED.\n", g_failures);
        return 1;
    }
}

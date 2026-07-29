/* Channel-level sanity checks for the YM2612 core (milestone 2b): algorithm
 * routing, operator-1 self-feedback, and the "untouched" silent case. */

#include <stdio.h>
#include <stdlib.h>
#include "ym2612.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static void configure_operator(Ym2612Operator *op, uint16_t fnum, uint8_t block, uint8_t tl) {
    ym2612_operator_init(op);
    ym2612_operator_set_freq(op, fnum, block);
    ym2612_operator_set_dt_mul(op, 0x01);   /* DT=0, MUL=1 */
    ym2612_operator_set_tl(op, tl);
    ym2612_operator_set_ar_ksr(op, 0x1F);   /* instant to full volume */
    ym2612_operator_set_d1r(op, 0x00);
    ym2612_operator_set_d2r(op, 0x00);
    ym2612_operator_set_sl_rr(op, 0x00);    /* SL=0 -> straight to held sustain */
}

/* Algorithm 7 makes all 4 operators independent, unmodulated carriers. Four
 * identical operators (same freq/phase/envelope, no modulation between
 * them) should sum to roughly 4x a single equivalent standalone operator's
 * peak amplitude. */
static void test_algorithm7_sums_identical_carriers(void) {
    Ym2612Channel ch;
    Ym2612Operator solo;
    int32_t channel_peak = 0, solo_peak = 0;
    int i;
    const int N = 400;

    ym2612_channel_init(&ch);
    ym2612_channel_set_algorithm(&ch, 0x07); /* algo=7, feedback=0 */
    for (i = 0; i < 4; i++) {
        configure_operator(&ch.op[i], 700, 4, 0x20);
    }
    ym2612_channel_key_on(&ch, 0x0F);

    configure_operator(&solo, 700, 4, 0x20);
    ym2612_operator_key_on(&solo);

    for (i = 0; i < N; i++) {
        int32_t cs = ym2612_channel_clock(&ch, 0, 0);
        int32_t ss = ym2612_operator_clock(&solo);
        if (iabs32(cs) > channel_peak) channel_peak = iabs32(cs);
        if (iabs32(ss) > solo_peak) solo_peak = iabs32(ss);
    }

    CHECK(solo_peak > 0, "solo reference operator actually produces sound");
    CHECK(iabs32(channel_peak - 4 * solo_peak) <= solo_peak / 4,
          "algorithm 7's 4 identical unmodulated carriers sum to ~4x a single operator's peak");
}

/* Operator-1 self-feedback should audibly change its waveform: same freq
 * and envelope, feedback off vs. maxed, must diverge sample-by-sample. */
static void test_feedback_changes_waveform(void) {
    Ym2612Channel ch_no_fb, ch_fb;
    int i;
    int differs = 0;
    const int N = 200;

    ym2612_channel_init(&ch_no_fb);
    ym2612_channel_set_algorithm(&ch_no_fb, 0x07); /* algo 7, FB=0 */
    configure_operator(&ch_no_fb.op[0], 700, 4, 0x10);
    ym2612_channel_key_on(&ch_no_fb, 0x01); /* only OP1 sounds */

    ym2612_channel_init(&ch_fb);
    ym2612_channel_set_algorithm(&ch_fb, 0x3F); /* algo 7, FB=7 (max) */
    configure_operator(&ch_fb.op[0], 700, 4, 0x10);
    ym2612_channel_key_on(&ch_fb, 0x01);

    for (i = 0; i < N; i++) {
        int32_t a = ym2612_channel_clock(&ch_no_fb, 0, 0);
        int32_t b = ym2612_channel_clock(&ch_fb, 0, 0);
        if (a != b) differs = 1;
    }

    CHECK(differs, "max self-feedback produces a different waveform than no feedback");
}

/* Different algorithms wired to the same operator registers must actually
 * produce different output -- proving the routing table is live, not a
 * no-op that always behaves like plain addition. */
static void test_different_algorithms_produce_different_output(void) {
    Ym2612Channel ch_algo0, ch_algo7;
    int i;
    int differs = 0;
    const int N = 200;

    ym2612_channel_init(&ch_algo0);
    ym2612_channel_set_algorithm(&ch_algo0, 0x00); /* algo 0: full 1>2>3>4 chain */
    for (i = 0; i < 4; i++) configure_operator(&ch_algo0.op[i], 700, 4, 0x08);
    ym2612_channel_key_on(&ch_algo0, 0x0F);

    ym2612_channel_init(&ch_algo7);
    ym2612_channel_set_algorithm(&ch_algo7, 0x07); /* algo 7: additive */
    for (i = 0; i < 4; i++) configure_operator(&ch_algo7.op[i], 700, 4, 0x08);
    ym2612_channel_key_on(&ch_algo7, 0x0F);

    for (i = 0; i < N; i++) {
        int32_t a = ym2612_channel_clock(&ch_algo0, 0, 0);
        int32_t b = ym2612_channel_clock(&ch_algo7, 0, 0);
        if (a != b) differs = 1;
    }

    CHECK(differs, "algorithm 0 (chained) and algorithm 7 (additive) sound different with identical registers");
}

/* A channel with every operator left un-keyed should be silent -- pure
 * regression guard against accidentally leaking a nonzero default output. */
static void test_unkeyed_channel_is_silent(void) {
    Ym2612Channel ch;
    int i;
    int all_zero = 1;

    ym2612_channel_init(&ch);
    ym2612_channel_set_algorithm(&ch, 0x07);
    for (i = 0; i < 4; i++) configure_operator(&ch.op[i], 700, 4, 0x08);
    /* deliberately never key anything on */

    for (i = 0; i < 100; i++) {
        if (ym2612_channel_clock(&ch, 0, 0) != 0) all_zero = 0;
    }

    CHECK(all_zero, "a channel with no operators keyed on stays silent");
}

int main(void) {
    test_algorithm7_sums_identical_carriers();
    test_feedback_changes_waveform();
    test_different_algorithms_produce_different_output();
    test_unkeyed_channel_is_silent();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d test(s) FAILED.\n", g_failures);
        return 1;
    }
}

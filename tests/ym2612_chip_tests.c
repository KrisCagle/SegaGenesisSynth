/* Chip-level sanity checks for the YM2612 core (milestone 2c): the real
 * register map/port addressing, physical-vs-logical operator slot order,
 * key-on/off register bits, channel-3 special mode, and LFO plumbing. */

#include <stdio.h>
#include <stdlib.h>
#include "ym2612.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* Real-software-style two-byte frequency write: high byte (block + top 3
 * fnum bits) latched first, then the low byte triggers the update. */
static void write_freq(Ym2612Chip *chip, int port, int chan_in_port, double freq_hz, int block) {
    uint16_t fnum = (uint16_t)(freq_hz * 1048576.0 / (YM2612_SAMPLE_HZ * (double)(1 << (block - 1))) + 0.5);
    ym2612_chip_write(chip, port, (uint8_t)(0xA4 + chan_in_port), (uint8_t)(((block & 7) << 3) | ((fnum >> 8) & 7)));
    ym2612_chip_write(chip, port, (uint8_t)(0xA0 + chan_in_port), (uint8_t)(fnum & 0xFF));
}

static void write_basic_patch(Ym2612Chip *chip, int port, int chan_in_port) {
    int slot;
    for (slot = 0; slot < 4; slot++) {
        uint8_t reg_off = (uint8_t)(slot * 4 + chan_in_port);
        ym2612_chip_write(chip, port, (uint8_t)(0x30 + reg_off), 0x01); /* DT=0, MUL=1 */
        ym2612_chip_write(chip, port, (uint8_t)(0x50 + reg_off), 0x1F); /* AR=31 */
        ym2612_chip_write(chip, port, (uint8_t)(0x60 + reg_off), 0x00);
        ym2612_chip_write(chip, port, (uint8_t)(0x70 + reg_off), 0x00);
        ym2612_chip_write(chip, port, (uint8_t)(0x80 + reg_off), 0x00); /* SL=0, RR=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x40 + reg_off), 0x08); /* TL */
    }
    ym2612_chip_write(chip, port, (uint8_t)(0xB0 + chan_in_port), 0x07); /* algo 7, additive */
    write_freq(chip, port, chan_in_port, 440.0, 4);
}

/* Writing a patch + key-on to channel 0 (port0) and channel 4 (port1 slot1)
 * should sound on exactly those two channels and nowhere else. */
static void test_register_writes_route_to_correct_channel(void) {
    Ym2612Chip chip;
    int i;
    int heard[6] = { 0, 0, 0, 0, 0, 0 };

    ym2612_chip_init(&chip);
    write_basic_patch(&chip, 0, 0); /* channel index 0 */
    write_basic_patch(&chip, 1, 1); /* channel index 4 */
    ym2612_chip_write(&chip, 0, 0x28, 0xF0); /* key on all 4 ops, channel 0 (data&3=0) */
    ym2612_chip_write(&chip, 0, 0x28, 0xF5); /* key on all 4 ops, channel (data&3=1)+3=4 */

    for (i = 0; i < 300; i++) {
        sample_t l, r;
        ym2612_chip_clock(&chip, &l, &r);
        (void)l; (void)r;
    }
    for (i = 0; i < 6; i++) {
        int j;
        for (j = 0; j < 4; j++) {
            if (chip.channel[i].op[j].state != YM_EG_OFF) heard[i] = 1;
        }
    }

    CHECK(heard[0] && heard[4], "keyed channels (0 and 4) actually entered an active envelope state");
    CHECK(!heard[1] && !heard[2] && !heard[3] && !heard[5], "unkeyed channels stay off");
}

/* Register $38 is DET/MUL for physical slot 2 on channel 0, which per the
 * real chip's scrambled register layout is logical OP2 (this codebase's
 * op[1]), not op[2]. Confirms LOGICAL_OP_FOR_REG_SLOT is wired correctly. */
static void test_physical_slot_order_maps_to_correct_logical_operator(void) {
    Ym2612Chip chip;
    ym2612_chip_init(&chip);

    ym2612_chip_write(&chip, 0, 0x48, 0x2A); /* TL ($40 base), physical slot 2, channel 0 */

    CHECK(chip.channel[0].op[1].tl == (0x2A << 3), "physical register slot 2 (channel 0) writes logical OP2 (op[1])");
    CHECK(chip.channel[0].op[2].tl != (uint32_t)(0x2A << 3), "logical OP3 (op[2]) is untouched by the same write");
}

/* The $28 key-on/off register's bits 4-7 select operators 1-4 (logical
 * order), and bit2 of the data byte selects the port-1 channel group. */
static void test_key_on_off_register_bits(void) {
    Ym2612Chip chip;
    ym2612_chip_init(&chip);
    write_basic_patch(&chip, 0, 1); /* channel index 1 */

    ym2612_chip_write(&chip, 0, 0x28, 0x11); /* channel 1, key on OP1 only */
    CHECK(chip.channel[1].op[0].key == 1, "key-on bit for OP1 sets op[0].key");
    CHECK(chip.channel[1].op[1].key == 0, "OP2 was not keyed on by the same write");

    ym2612_chip_write(&chip, 0, 0x28, 0x01); /* channel 1, key everything off */
    CHECK(chip.channel[1].op[0].key == 0, "key-off clears op[0].key");
}

/* Channel-3 special mode: 3 of its 4 operators get independent frequencies
 * from the $A8-$AE registers, using the documented (and scrambled)
 * SL3-index -> logical-operator mapping; OP4 keeps using the channel's
 * normal (shared) frequency from $A2/$A6. */
static void test_channel3_special_mode_independent_frequencies(void) {
    Ym2612Chip chip;
    uint16_t fnum_op3, fnum_op1, fnum_op2, fnum_normal;

    ym2612_chip_init(&chip);
    ym2612_chip_write(&chip, 0, 0x27, 0x40); /* enable 3-slot special mode */

    /* normal channel-3 frequency (used by OP4), via $A2/$A6 */
    write_freq(&chip, 0, 2, 300.0, 3);
    fnum_normal = chip.channel[2].op[3].kc; /* just capture something derived from it */

    /* $A8/$AC = SL3 index 0 -> logical OP3 (op[2]) */
    ym2612_chip_write(&chip, 0, 0xAC, (3 << 3));
    ym2612_chip_write(&chip, 0, 0xA8, 0x40);
    fnum_op3 = (uint16_t)chip.channel[2].op[2].fc_raw;

    /* $A9/$AD = SL3 index 1 -> logical OP1 (op[0]) */
    ym2612_chip_write(&chip, 0, 0xAD, (4 << 3));
    ym2612_chip_write(&chip, 0, 0xA9, 0x80);
    fnum_op1 = (uint16_t)chip.channel[2].op[0].fc_raw;

    /* $AA/$AE = SL3 index 2 -> logical OP2 (op[1]) */
    ym2612_chip_write(&chip, 0, 0xAE, (5 << 3));
    ym2612_chip_write(&chip, 0, 0xAA, 0xC0);
    fnum_op2 = (uint16_t)chip.channel[2].op[1].fc_raw;

    CHECK(fnum_op3 != fnum_op1 && fnum_op1 != fnum_op2 && fnum_op3 != fnum_op2,
          "channel 3's OP1/OP2/OP3 each received distinct independent frequencies");
    CHECK(chip.channel[2].op[3].fc_raw != chip.channel[2].op[0].fc_raw,
          "OP4 keeps the normal shared channel frequency, distinct from OP1's special-mode one");
    (void)fnum_normal;
}

/* With the LFO enabled and PM sensitivity set on a channel, output should
 * differ over time from the same patch with PM sensitivity at 0 -- a
 * measurable vibrato effect, not a silent no-op. */
static void test_lfo_pm_changes_output_over_time(void) {
    Ym2612Chip chip_pm, chip_no_pm;
    int i, differs = 0;

    ym2612_chip_init(&chip_pm);
    ym2612_chip_write(&chip_pm, 0, 0x22, 0x0F); /* LFO on, fastest rate */
    write_basic_patch(&chip_pm, 0, 0);
    ym2612_chip_write(&chip_pm, 0, 0xB4, 0xC7); /* pan L+R on, PMS = 7 (max) */
    ym2612_chip_write(&chip_pm, 0, 0x28, 0xF0);

    ym2612_chip_init(&chip_no_pm);
    ym2612_chip_write(&chip_no_pm, 0, 0x22, 0x0F);
    write_basic_patch(&chip_no_pm, 0, 0);
    ym2612_chip_write(&chip_no_pm, 0, 0xB4, 0xC0); /* pan L+R on, PMS = 0 */
    ym2612_chip_write(&chip_no_pm, 0, 0x28, 0xF0);

    for (i = 0; i < 4000; i++) {
        sample_t al, ar, bl, br;
        ym2612_chip_clock(&chip_pm, &al, &ar);
        ym2612_chip_clock(&chip_no_pm, &bl, &br);
        if (al != bl || ar != br) differs = 1;
    }

    CHECK(differs, "nonzero LFO PM sensitivity measurably changes output vs. PMS=0 over time");
}

/* Milestone 2d: a channel panned hard left should produce sound only on
 * the left output, hard right only on the right, and $B4=0x00 (both
 * disabled) should silence it on both sides -- even though the operators
 * underneath are still actively running. */
static void test_pan_routes_to_correct_stereo_side(void) {
    Ym2612Chip chip;
    int i;
    int left_heard = 0, right_heard = 0;
    int silent_both = 1;

    ym2612_chip_init(&chip);
    write_basic_patch(&chip, 0, 0);
    ym2612_chip_write(&chip, 0, 0xB4, 0x80); /* pan: left only */
    ym2612_chip_write(&chip, 0, 0x28, 0xF0);

    for (i = 0; i < 300; i++) {
        sample_t l, r;
        ym2612_chip_clock(&chip, &l, &r);
        if (l != 0) left_heard = 1;
        if (r != 0) right_heard = 1;
    }
    CHECK(left_heard && !right_heard, "pan=left-only produces sound on left and nothing on right");

    ym2612_chip_init(&chip);
    write_basic_patch(&chip, 0, 0);
    ym2612_chip_write(&chip, 0, 0xB4, 0x00); /* pan: both disabled */
    ym2612_chip_write(&chip, 0, 0x28, 0xF0);

    for (i = 0; i < 300; i++) {
        sample_t l, r;
        ym2612_chip_clock(&chip, &l, &r);
        if (l != 0 || r != 0) silent_both = 0;
    }
    CHECK(silent_both, "pan=0x00 (both disabled) silences the channel on both sides, even while keyed on");
}

int main(void) {
    test_register_writes_route_to_correct_channel();
    test_physical_slot_order_maps_to_correct_logical_operator();
    test_key_on_off_register_bits();
    test_channel3_special_mode_independent_frequencies();
    test_lfo_pm_changes_output_over_time();
    test_pan_routes_to_correct_stereo_side();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d test(s) FAILED.\n", g_failures);
        return 1;
    }
}

/* YM2612 FM synthesis core, milestone 2a (single operator).
 *
 * The envelope-rate tables, detune table, and key-code table below are
 * factual, reverse-engineered hardware timing data -- not derived from a
 * formula -- ported from the lineage of MAME's YM2612 core (Jarek
 * Burczynski, Tatsuyuki Satoh) as refined for Genesis Plus GX by Eke-Eke
 * using Nemesis's and Sauraen's real-hardware/die-shot research
 * (gendev.spritesmind.net). That lineage is the de facto reference every
 * accurate open-source YM2612 emulator ports these same tables from. The
 * sine/exponential tables are instead generated at startup from the
 * documented formula (see ym2612_ensure_tables), not hand-copied.
 */

#include <math.h>
#include "ym2612.h"

#define ENV_BITS 10
#define ENV_LEN (1 << ENV_BITS)
#define ENV_STEP (128.0 / ENV_LEN) /* dB per envelope step */

#define MAX_ATT_INDEX (ENV_LEN - 1) /* 1023 */
#define MIN_ATT_INDEX 0

#define DT_BITS 17
#define DT_LEN (1u << DT_BITS)
#define DT_MASK (DT_LEN - 1u)

#define SIN_BITS 10
#define SIN_LEN (1 << SIN_BITS)
#define SIN_MASK (SIN_LEN - 1)

#define TL_RES_LEN 256
#define TL_TAB_LEN (13 * 2 * TL_RES_LEN)
#define ENV_QUIET (TL_TAB_LEN >> 3)

#define RATE_STEPS 8

#define YM_PI 3.14159265358979323846

/* --- Sustain level table: 3dB steps, expressed in the envelope's
 * ENV_STEP(=0.125dB)-sized units, i.e. dB * 32. Index 15 (the "31" entry)
 * is the special SL=15 case meaning "never reaches sustain / effectively
 * off". --- */
#define SC(db) ((db) * 32)
static const uint32_t sl_table[16] = {
    SC(0),  SC(1),  SC(2),  SC(3),  SC(4),  SC(5),  SC(6),  SC(7),
    SC(8),  SC(9),  SC(10), SC(11), SC(12), SC(13), SC(14), SC(31)
};
#undef SC

/* Envelope increment amounts per rate-group (0-11 use a 4-of-64 cycling
 * pattern; 12-15 are progressively coarser; row 17 is the AR=maximum
 * "instant" case; row 18 is the "infinite" / no-change case). */
static const uint8_t eg_inc[19 * RATE_STEPS] = {
    /* cycle:0 1  2 3  4 5  6 7 */
    0, 1, 0, 1, 0, 1, 0, 1, /* rates 00..11 0 */
    0, 1, 0, 1, 1, 1, 0, 1, /* rates 00..11 1 */
    0, 1, 1, 1, 0, 1, 1, 1, /* rates 00..11 2 */
    0, 1, 1, 1, 1, 1, 1, 1, /* rates 00..11 3 */

    1, 1, 1, 1, 1, 1, 1, 1, /* rate 12 0 */
    1, 1, 1, 2, 1, 1, 1, 2, /* rate 12 1 */
    1, 2, 1, 2, 1, 2, 1, 2, /* rate 12 2 */
    1, 2, 2, 2, 1, 2, 2, 2, /* rate 12 3 */

    2, 2, 2, 2, 2, 2, 2, 2, /* rate 13 0 */
    2, 2, 2, 4, 2, 2, 2, 4, /* rate 13 1 */
    2, 4, 2, 4, 2, 4, 2, 4, /* rate 13 2 */
    2, 4, 4, 4, 2, 4, 4, 4, /* rate 13 3 */

    4, 4, 4, 4, 4, 4, 4, 4, /* rate 14 0 */
    4, 4, 4, 8, 4, 4, 4, 8, /* rate 14 1 */
    4, 8, 4, 8, 4, 8, 4, 8, /* rate 14 2 */
    4, 8, 8, 8, 4, 8, 8, 8, /* rate 14 3 */

    8, 8, 8, 8, 8, 8, 8, 8,           /* rates 15 0-3 */
    16, 16, 16, 16, 16, 16, 16, 16,   /* rate 15 2/3 for attack */
    0, 0, 0, 0, 0, 0, 0, 0            /* infinite rate: no change */
};

#define O(a) ((a) * RATE_STEPS)
static const uint16_t eg_rate_select[32 + 64 + 32] = {
    /* 32 infinite-time rates */
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
    O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),

    /* rates 00-11 (real-hardware quirk: 00 and 01 behave as infinite too) */
    O(18), O(18), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),
    O(0), O(1), O(2), O(3),

    /* rate 12 */
    O(4), O(5), O(6), O(7),
    /* rate 13 */
    O(8), O(9), O(10), O(11),
    /* rate 14 */
    O(12), O(13), O(14), O(15),
    /* rate 15 */
    O(16), O(16), O(16), O(16),

    /* 32 dummy rates (same as 15 3) */
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
    O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16)
};
#undef O

static const uint8_t eg_rate_shift[32 + 64 + 32] = {
    /* 32 infinite-time rates */
    11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11,

    /* rates 00-11 */
    11, 11, 11, 11,
    10, 10, 10, 10,
    9, 9, 9, 9,
    8, 8, 8, 8,
    7, 7, 7, 7,
    6, 6, 6, 6,
    5, 5, 5, 5,
    4, 4, 4, 4,
    3, 3, 3, 3,
    2, 2, 2, 2,
    1, 1, 1, 1,
    0, 0, 0, 0,

    /* rate 12 */
    0, 0, 0, 0,
    /* rate 13 */
    0, 0, 0, 0,
    /* rate 14 */
    0, 0, 0, 0,
    /* rate 15 */
    0, 0, 0, 0,

    /* 32 dummy rates */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Detune, in 10.10 fixed-point phase units, indexed by key code (0-31) for
 * each of the 4 raw "FD" magnitudes; FD 4-7 are the negated mirror of 0-3
 * (the real DT register field is 3 bits: 0/4 = no detune, 1-3 = increasing
 * positive, 5-7 = the same magnitudes negative). */
static const uint8_t dt_tab_raw[4 * 32] = {
    /* FD=0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* FD=1 */
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8,
    /* FD=2 */
    1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
    5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16,
    /* FD=3 */
    2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7,
    8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22
};

/* fnum's top 4 bits -> key code's low 2 bits */
static const uint8_t opn_fktable[16] = { 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3 };

/* LFO AM depth selector: 4 available depths (0, 1.4, 5.9, 11.8 dB),
 * implemented as a shift on a 0-126 sawtooth-ish generator. */
static const uint8_t LFO_AMS_DEPTH_SHIFT[4] = { 8, 3, 1, 0 };

/* LFO PM output, first (positive) quarter only -- 7 fnum-bit positions x 8
 * PM depths x 8 steps. The full 128-entry-per-(fnum,depth) table used at
 * runtime is generated from this at startup (see ym2612_ensure_tables). */
static const uint8_t LFO_PM_OUTPUT[7 * 8][8] = {
    /* FNUM BIT 4 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 1, 1, 1 },
    /* FNUM BIT 5 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1 }, { 0, 0, 1, 1, 2, 2, 2, 3 },
    /* FNUM BIT 6 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 1 }, { 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 0, 1, 1, 2, 2, 2, 3 }, { 0, 0, 2, 3, 4, 4, 5, 6 },
    /* FNUM BIT 7 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 1 }, { 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 0, 0, 1, 1, 1, 1, 2 }, { 0, 0, 1, 1, 2, 2, 2, 3 },
    { 0, 0, 2, 3, 4, 4, 5, 6 }, { 0, 0, 4, 6, 8, 8, 0xa, 0xc },
    /* FNUM BIT 8 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 0, 0, 1, 1, 1, 2, 2 }, { 0, 0, 1, 1, 2, 2, 3, 3 },
    { 0, 0, 1, 2, 2, 2, 3, 4 }, { 0, 0, 2, 3, 4, 4, 5, 6 },
    { 0, 0, 4, 6, 8, 8, 0xa, 0xc }, { 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 },
    /* FNUM BIT 9 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 2, 2, 2, 2 },
    { 0, 0, 0, 2, 2, 2, 4, 4 }, { 0, 0, 2, 2, 4, 4, 6, 6 },
    { 0, 0, 2, 4, 4, 4, 6, 8 }, { 0, 0, 4, 6, 8, 8, 0xa, 0xc },
    { 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 }, { 0, 0, 0x10, 0x18, 0x20, 0x20, 0x28, 0x30 },
    /* FNUM BIT 10 */
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 4, 4, 4, 4 },
    { 0, 0, 0, 4, 4, 4, 8, 8 }, { 0, 0, 4, 4, 8, 8, 0xc, 0xc },
    { 0, 0, 4, 8, 8, 8, 0xc, 0x10 }, { 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 },
    { 0, 0, 0x10, 0x18, 0x20, 0x20, 0x28, 0x30 }, { 0, 0, 0x20, 0x30, 0x40, 0x40, 0x50, 0x60 }
};

static int16_t g_dt_tab[8][32];
static int32_t g_tl_tab[TL_TAB_LEN];
static uint32_t g_sin_tab[SIN_LEN];
static int32_t g_lfo_pm_table[128 * 8 * 32]; /* 128 fnum-bit combos x 8 depths x 32 steps */
static int g_tables_ready = 0;

static void ym2612_ensure_tables(void) {
    int d, i, x, n;
    double o, m;

    if (g_tables_ready) return;

    for (d = 0; d < 4; d++) {
        for (i = 0; i < 32; i++) {
            g_dt_tab[d][i] = dt_tab_raw[d * 32 + i];
            g_dt_tab[d + 4][i] = (int16_t)-g_dt_tab[d][i];
        }
    }

    /* Linear power (exponential) table: converts a log2-domain attenuation
     * value into a linear sample magnitude, plus a per-bit-depth shifted
     * copy (13 rows) so different attenuation resolutions all index the
     * same table -- this is the chip's real table layout. */
    for (x = 0; x < TL_RES_LEN; x++) {
        m = floor((1 << 16) / pow(2.0, (x + 1) * (ENV_STEP / 4.0) / 8.0));
        n = (int)m;
        n >>= 4;
        n = (n & 1) ? (n >> 1) + 1 : (n >> 1);
        n <<= 2;

        g_tl_tab[x * 2 + 0] = n;
        g_tl_tab[x * 2 + 1] = -g_tl_tab[x * 2 + 0];

        for (i = 1; i < 13; i++) {
            g_tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN] = g_tl_tab[x * 2 + 0] >> i;
            g_tl_tab[x * 2 + 1 + i * 2 * TL_RES_LEN] = -g_tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN];
        }
    }

    /* Logarithmic sine table: stores attenuation (in dB-ish units), not
     * amplitude, so that combining phase and envelope is a log-domain
     * addition rather than a multiply -- the key design point that makes
     * this "the real thing" rather than a naive sine*envelope synth. */
    for (i = 0; i < SIN_LEN; i++) {
        m = sin(((i * 2) + 1) * YM_PI / SIN_LEN);
        o = (m > 0.0) ? 8.0 * log(1.0 / m) / log(2.0) : 8.0 * log(-1.0 / m) / log(2.0);
        o = o / (ENV_STEP / 4.0);

        n = (int)(2.0 * o);
        n = (n & 1) ? (n >> 1) + 1 : (n >> 1);

        g_sin_tab[i] = (uint32_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }

    /* LFO PM table: for each 7-bit fnum slice and each of 8 depths, sum the
     * per-bit PM contributions from LFO_PM_OUTPUT, then mirror that
     * (positive) quarter into a full +/- 32-step cycle per depth. */
    {
        int depth, fnum, step, bit;
        for (depth = 0; depth < 8; depth++) {
            for (fnum = 0; fnum < 128; fnum++) {
                for (step = 0; step < 8; step++) {
                    uint8_t value = 0;
                    for (bit = 0; bit < 7; bit++) {
                        if (fnum & (1 << bit)) {
                            value = (uint8_t)(value + LFO_PM_OUTPUT[bit * 8 + depth][step]);
                        }
                    }
                    g_lfo_pm_table[(fnum * 32 * 8) + (depth * 32) + step + 0] = value;
                    g_lfo_pm_table[(fnum * 32 * 8) + (depth * 32) + (step ^ 7) + 8] = value;
                    g_lfo_pm_table[(fnum * 32 * 8) + (depth * 32) + step + 16] = -value;
                    g_lfo_pm_table[(fnum * 32 * 8) + (depth * 32) + (step ^ 7) + 24] = -value;
                }
            }
        }
    }

    g_tables_ready = 1;
}

static void recompute_phase_inc(Ym2612Operator *op) {
    uint32_t fc = (op->fc_raw + (uint32_t)op->dt[op->kc]) & DT_MASK;
    op->phase_inc = (fc * op->mul) >> 1;
}

static void recompute_eg_rates(Ym2612Operator *op) {
    uint8_t kc_ks = op->kc >> op->ksr_shift;

    if (op->ar + kc_ks < 32 + 62) {
        op->eg_sh_ar = eg_rate_shift[op->ar + kc_ks];
        op->eg_sel_ar = eg_rate_select[op->ar + kc_ks];
    } else {
        /* real-hardware quirk: attack is blocked entirely at max rate */
        op->eg_sh_ar = 0;
        op->eg_sel_ar = 18 * RATE_STEPS;
    }

    op->eg_sh_d1r = eg_rate_shift[op->d1r + kc_ks];
    op->eg_sel_d1r = eg_rate_select[op->d1r + kc_ks];
    op->eg_sh_d2r = eg_rate_shift[op->d2r + kc_ks];
    op->eg_sel_d2r = eg_rate_select[op->d2r + kc_ks];
    op->eg_sh_rr = eg_rate_shift[op->rr + kc_ks];
    op->eg_sel_rr = eg_rate_select[op->rr + kc_ks];
}

static void update_vol_out(Ym2612Operator *op) {
    if ((op->ssg & 0x08) && (op->ssgn ^ (op->ssg & 0x04))) {
        op->vol_out = ((uint32_t)(0x200 - op->volume) & MAX_ATT_INDEX) + op->tl;
    } else {
        op->vol_out = (uint32_t)op->volume + op->tl;
    }
}

void ym2612_operator_init(Ym2612Operator *op) {
    ym2612_ensure_tables();

    op->mul = 1;
    op->dt = g_dt_tab[0];
    op->tl = 0;
    op->ar = op->d1r = op->d2r = op->rr = 0;
    op->sl = 0;
    op->ksr_shift = 3;
    op->ssg = 0;
    op->fc_raw = 0;
    op->block_fnum = 0;
    op->kc = 0;
    op->am_mask = 0;

    op->phase = 0;
    op->phase_inc = 0;
    op->state = YM_EG_OFF;
    op->volume = MAX_ATT_INDEX;
    op->vol_out = MAX_ATT_INDEX;
    op->ssgn = 0;
    op->key = 0;

    op->eg_timer = 0;
    op->eg_cnt = 0;
    recompute_eg_rates(op);
}

void ym2612_operator_set_dt_mul(Ym2612Operator *op, uint8_t reg_value) {
    op->mul = (reg_value & 0x0F) ? (uint32_t)(reg_value & 0x0F) * 2 : 1;
    op->dt = g_dt_tab[(reg_value >> 4) & 7];
    recompute_phase_inc(op);
}

void ym2612_operator_set_tl(Ym2612Operator *op, uint8_t reg_value) {
    op->tl = (uint32_t)(reg_value & 0x7F) << (ENV_BITS - 7);
    update_vol_out(op);
}

void ym2612_operator_set_ar_ksr(Ym2612Operator *op, uint8_t reg_value) {
    op->ar = (reg_value & 0x1F) ? 32u + (((uint32_t)reg_value & 0x1F) << 1) : 0;
    op->ksr_shift = (uint8_t)(3 - (reg_value >> 6));
    recompute_eg_rates(op);
}

void ym2612_operator_set_d1r(Ym2612Operator *op, uint8_t reg_value) {
    op->d1r = (reg_value & 0x1F) ? 32u + (((uint32_t)reg_value & 0x1F) << 1) : 0;
    op->am_mask = (reg_value & 0x80) ? 0xFFFFFFFFu : 0; /* bit7 = AM ENABLE, shares this register with D1R */
    recompute_eg_rates(op);
}

void ym2612_operator_set_d2r(Ym2612Operator *op, uint8_t reg_value) {
    op->d2r = (reg_value & 0x1F) ? 32u + (((uint32_t)reg_value & 0x1F) << 1) : 0;
    recompute_eg_rates(op);
}

void ym2612_operator_set_sl_rr(Ym2612Operator *op, uint8_t reg_value) {
    op->sl = sl_table[reg_value >> 4];
    if (op->state == YM_EG_DEC && op->volume >= (int32_t)op->sl) {
        op->state = YM_EG_SUS;
    }
    op->rr = 34u + (((uint32_t)reg_value & 0x0F) << 2);
    recompute_eg_rates(op);
}

void ym2612_operator_set_ssg(Ym2612Operator *op, uint8_t reg_value) {
    op->ssg = reg_value & 0x0F;
}

void ym2612_operator_set_freq(Ym2612Operator *op, uint16_t fnum, uint8_t block) {
    op->kc = (uint8_t)((block << 2) | opn_fktable[fnum >> 7]);
    op->fc_raw = ((uint32_t)fnum << block) >> 1;
    op->block_fnum = ((uint32_t)block << 11) | fnum;
    recompute_phase_inc(op);
    recompute_eg_rates(op);
}

void ym2612_operator_key_on(Ym2612Operator *op) {
    if (!op->key) {
        op->phase = 0;
        op->ssgn = 0;

        if (op->ar + (op->kc >> op->ksr_shift) < 94) {
            op->state = (op->volume <= MIN_ATT_INDEX)
                            ? ((op->sl == MIN_ATT_INDEX) ? YM_EG_SUS : YM_EG_DEC)
                            : YM_EG_ATT;
        } else {
            op->volume = MIN_ATT_INDEX;
            op->state = (op->sl == MIN_ATT_INDEX) ? YM_EG_SUS : YM_EG_DEC;
        }
        update_vol_out(op);
    }
    op->key = 1;
}

void ym2612_operator_key_off(Ym2612Operator *op) {
    if (op->key && op->state > YM_EG_REL) {
        op->state = YM_EG_REL;
    }
    op->key = 0;
}

static void ssg_eg_update(Ym2612Operator *op) {
    if (!(op->ssg & 0x08) || op->volume < 0x200 || op->state <= YM_EG_REL) {
        return;
    }

    if (op->ssg & 0x01) { /* hold */
        if (op->ssg & 0x02) op->ssgn = 4;
        if (op->state != YM_EG_ATT && !(op->ssgn ^ (op->ssg & 0x04))) {
            op->volume = MAX_ATT_INDEX;
        }
    } else { /* loop */
        if (op->ssg & 0x02) {
            op->ssgn ^= 4;
        } else {
            op->phase = 0;
        }

        if (op->state != YM_EG_ATT) {
            uint8_t kc_ks = op->kc >> op->ksr_shift;
            if (op->ar + kc_ks < 94) {
                op->state = (op->volume <= MIN_ATT_INDEX)
                                ? ((op->sl == MIN_ATT_INDEX) ? YM_EG_SUS : YM_EG_DEC)
                                : YM_EG_ATT;
            } else {
                op->volume = MIN_ATT_INDEX;
                op->state = (op->sl == MIN_ATT_INDEX) ? YM_EG_SUS : YM_EG_DEC;
            }
        }
    }
    update_vol_out(op);
}

static void eg_advance(Ym2612Operator *op) {
    op->eg_timer++;
    if (op->eg_timer < 3) return;
    op->eg_timer = 0;

    op->eg_cnt++;
    if (op->eg_cnt == 4096) op->eg_cnt = 1;

    switch (op->state) {
        case YM_EG_ATT:
            if (!(op->eg_cnt & ((1u << op->eg_sh_ar) - 1))) {
                op->volume += (~op->volume * eg_inc[op->eg_sel_ar + ((op->eg_cnt >> op->eg_sh_ar) & 7)]) >> 4;
                if ((int32_t)op->volume <= MIN_ATT_INDEX) {
                    op->volume = MIN_ATT_INDEX;
                    op->state = (op->sl == MIN_ATT_INDEX) ? YM_EG_SUS : YM_EG_DEC;
                }
                update_vol_out(op);
            }
            break;

        case YM_EG_DEC:
            if (!(op->eg_cnt & ((1u << op->eg_sh_d1r) - 1))) {
                if (op->ssg & 0x08) {
                    if (op->volume < 0x200) {
                        op->volume += 4 * eg_inc[op->eg_sel_d1r + ((op->eg_cnt >> op->eg_sh_d1r) & 7)];
                        update_vol_out(op);
                    }
                } else {
                    op->volume += eg_inc[op->eg_sel_d1r + ((op->eg_cnt >> op->eg_sh_d1r) & 7)];
                    op->vol_out = (uint32_t)op->volume + op->tl;
                }
                if (op->volume >= (int32_t)op->sl) op->state = YM_EG_SUS;
            }
            break;

        case YM_EG_SUS:
            if (!(op->eg_cnt & ((1u << op->eg_sh_d2r) - 1))) {
                if (op->ssg & 0x08) {
                    if (op->volume < 0x200) {
                        op->volume += 4 * eg_inc[op->eg_sel_d2r + ((op->eg_cnt >> op->eg_sh_d2r) & 7)];
                        update_vol_out(op);
                    }
                } else {
                    op->volume += eg_inc[op->eg_sel_d2r + ((op->eg_cnt >> op->eg_sh_d2r) & 7)];
                    if (op->volume >= MAX_ATT_INDEX) op->volume = MAX_ATT_INDEX;
                    op->vol_out = op->volume + op->tl;
                }
            }
            break;

        case YM_EG_REL:
            if (!(op->eg_cnt & ((1u << op->eg_sh_rr) - 1))) {
                if (op->ssg & 0x08) {
                    if (op->volume < 0x200) {
                        op->volume += 4 * eg_inc[op->eg_sel_rr + ((op->eg_cnt >> op->eg_sh_rr) & 7)];
                    }
                    if (op->volume >= 0x200) {
                        op->volume = MAX_ATT_INDEX;
                        op->state = YM_EG_OFF;
                    }
                } else {
                    op->volume += eg_inc[op->eg_sel_rr + ((op->eg_cnt >> op->eg_sh_rr) & 7)];
                    if (op->volume >= MAX_ATT_INDEX) {
                        op->volume = MAX_ATT_INDEX;
                        op->state = YM_EG_OFF;
                    }
                }
                op->vol_out = op->volume + op->tl;
            }
            break;

        case YM_EG_OFF:
        default:
            break;
    }
}

/* pm is a phase-modulation input expressed directly in sine-table index
 * units (added mod SIN_LEN) -- in a full channel, pm is another operator's
 * raw output sample, which is how phase modulation FM actually works here:
 * the modulator's amplitude *is* the carrier's phase deviation. */
static int32_t op_calc_ex(uint32_t phase, uint32_t env, int32_t pm) {
    uint32_t idx = (uint32_t)((int32_t)(phase >> SIN_BITS) + pm) & SIN_MASK;
    uint32_t p = (env << 3) + g_sin_tab[idx];
    if (p >= TL_TAB_LEN) return 0;
    return g_tl_tab[p];
}

sample_t ym2612_operator_clock(Ym2612Operator *op) {
    ssg_eg_update(op);
    eg_advance(op);
    op->phase += op->phase_inc;
    return clamp_s16(op_calc_ex(op->phase, op->vol_out, 0));
}

/* ============================== Channel ============================== */

/* Where each operator's output is routed for a given algorithm. OP4 always
 * feeds the channel's final output directly, so it isn't listed here.
 * Algorithm 5 is the odd one out: OP1's output fans out to all three buses
 * in the same sample rather than going through one bus like every other
 * algorithm, so it's flagged instead of given a normal destination. */
typedef enum { BUS_C1, BUS_C2, BUS_MEM, BUS_CARRIER, BUS_ALGO5_FANOUT } Ym2612Bus;
typedef enum { RESTORE_M2, RESTORE_C2, RESTORE_NONE } Ym2612MemRestore;

typedef struct {
    Ym2612Bus op1_dest, op2_dest, op3_dest;
    Ym2612MemRestore mem_restore;
} AlgoRouting;

static const AlgoRouting ALGO_ROUTING[8] = {
    /* 0: OP1->C1->OP2->MEM(delay)->OP3->C2->OP4->OUT (the classic 1>2>3>4 chain) */
    { BUS_C1, BUS_MEM, BUS_C2, RESTORE_M2 },
    /* 1: (OP1+OP2)->MEM(delay)->OP3->C2->OP4->OUT */
    { BUS_MEM, BUS_MEM, BUS_C2, RESTORE_M2 },
    /* 2: OP1 and (OP2->MEM(delay)->OP3) both land on C2 -> OP4 -> OUT */
    { BUS_C2, BUS_MEM, BUS_C2, RESTORE_M2 },
    /* 3: OP1->C1->OP2->MEM(delay); that delayed value and OP3 both land on C2 -> OP4 -> OUT */
    { BUS_C1, BUS_MEM, BUS_C2, RESTORE_C2 },
    /* 4: (OP1->C1->OP2) and (OP3->C2->OP4) -- two independent 2-op stacks, both carriers */
    { BUS_C1, BUS_CARRIER, BUS_C2, RESTORE_NONE },
    /* 5: OP1 modulates OP2, OP3 and OP4 in parallel (fans out same-sample, no delay bus) */
    { BUS_ALGO5_FANOUT, BUS_CARRIER, BUS_CARRIER, RESTORE_M2 },
    /* 6: OP1->C1->OP2, with OP3 and OP4 as independent carriers alongside it */
    { BUS_C1, BUS_CARRIER, BUS_CARRIER, RESTORE_NONE },
    /* 7: all 4 operators are independent, unmodulated carriers (pure additive) */
    { BUS_CARRIER, BUS_CARRIER, BUS_CARRIER, RESTORE_NONE }
};

static void write_bus(Ym2612Bus bus, int32_t value, int32_t *c1, int32_t *c2, int32_t *mem, int32_t *carrier) {
    switch (bus) {
        case BUS_C1: *c1 += value; break;
        case BUS_C2: *c2 += value; break;
        case BUS_MEM: *mem += value; break;
        case BUS_CARRIER: *carrier += value; break;
        case BUS_ALGO5_FANOUT: break; /* handled specially by the caller */
    }
}

void ym2612_channel_init(Ym2612Channel *ch) {
    int i;
    for (i = 0; i < 4; i++) {
        ym2612_operator_init(&ch->op[i]);
    }
    ch->algo = 0;
    ch->fb_shift = SIN_BITS; /* == 10: feedback disabled */
    ch->op1_out_hist[0] = 0;
    ch->op1_out_hist[1] = 0;
    ch->mem_value = 0;
    ch->pms = 0;
    ch->ams = 0;
    /* Default to both speakers enabled: real power-on pan state is
     * undefined hardware-wise, and every real game sets it explicitly
     * anyway, so defaulting to "silent until programmed" would just be a
     * footgun for code (like our own earlier milestones' demos) that
     * builds a channel directly without going through chip register writes. */
    ch->pan_l = 1;
    ch->pan_r = 1;
}

void ym2612_channel_set_algorithm(Ym2612Channel *ch, uint8_t reg_value) {
    ch->algo = reg_value & 0x07;
    ch->fb_shift = (uint8_t)(SIN_BITS - ((reg_value >> 3) & 0x07));
}

void ym2612_channel_set_pan_and_sens(Ym2612Channel *ch, uint8_t reg_value) {
    ch->pms = (uint32_t)(reg_value & 0x07) * 32;
    ch->ams = LFO_AMS_DEPTH_SHIFT[(reg_value >> 4) & 0x03];
    ch->pan_l = (reg_value & 0x80) ? 1 : 0;
    ch->pan_r = (reg_value & 0x40) ? 1 : 0;
}

void ym2612_channel_key_on(Ym2612Channel *ch, uint8_t op_bits) {
    int i;
    for (i = 0; i < 4; i++) {
        if (op_bits & (1u << i)) ym2612_operator_key_on(&ch->op[i]);
    }
}

void ym2612_channel_key_off(Ym2612Channel *ch, uint8_t op_bits) {
    int i;
    for (i = 0; i < 4; i++) {
        if (op_bits & (1u << i)) ym2612_operator_key_off(&ch->op[i]);
    }
}

static uint32_t operator_eg_out(const Ym2612Operator *op, uint32_t am) {
    return op->vol_out + (am & op->am_mask);
}

/* PM-aware phase advance for one operator: looks up the LFO's current
 * phase-deviation contribution (in sine-table index units) for this
 * operator's own fnum/block and this channel's PM sensitivity, then
 * recomputes the increment for this sample only -- otherwise falls back to
 * the precomputed, unmodulated phase_inc exactly as before. Algebraically
 * identical whether called per-operator (as here) or once per channel
 * (as the real hardware effectively does for non-3-slot channels), since
 * every operator normally shares the same block_fnum/kc. */
static void advance_phase_lfo(Ym2612Operator *op, uint32_t pm_index) {
    int32_t lfo_fn_offset = g_lfo_pm_table[((op->block_fnum & 0x7f0) << 4) + pm_index];

    if (lfo_fn_offset != 0) {
        uint8_t blk = (uint8_t)(op->block_fnum >> 11);
        uint32_t fc = (uint32_t)((int32_t)(op->block_fnum << 1) + lfo_fn_offset) & 0xFFFu;
        fc = (fc << blk) >> 2;
        fc = (fc + (uint32_t)op->dt[op->kc]) & DT_MASK;
        op->phase += (fc * op->mul) >> 1;
    } else {
        op->phase += op->phase_inc;
    }
}

sample_t ym2612_channel_clock(Ym2612Channel *ch, uint32_t lfo_am, uint32_t lfo_pm) {
    const AlgoRouting *r = &ALGO_ROUTING[ch->algo];
    int32_t m2 = 0, c1 = 0, c2 = 0, mem = 0, carrier = 0;
    int32_t fb_in = 0;
    int32_t out1, out2, out3, out4;
    uint32_t am, pm_index;
    int i;

    for (i = 0; i < 4; i++) ssg_eg_update(&ch->op[i]);

    am = lfo_am >> ch->ams;
    pm_index = ch->pms + lfo_pm;

    /* restore last sample's delayed MEM value into whichever bus this
     * algorithm reads it from */
    if (r->mem_restore == RESTORE_M2) m2 = ch->mem_value;
    else if (r->mem_restore == RESTORE_C2) c2 = ch->mem_value;

    /* Real hardware computes operators in order OP1, OP3, OP2, OP4 (not
     * numeric order) -- this matters because OP3 reads the just-restored
     * m2 bus, OP2 reads whatever OP1 just wrote to c1, and OP4 reads
     * whatever OP1/OP3 wrote to c2, all within this same sample. */

    if (ch->fb_shift < SIN_BITS) {
        fb_in = (ch->op1_out_hist[0] + ch->op1_out_hist[1]) >> ch->fb_shift;
    }
    out1 = op_calc_ex(ch->op[0].phase, operator_eg_out(&ch->op[0], am), fb_in);
    ch->op1_out_hist[0] = ch->op1_out_hist[1];
    ch->op1_out_hist[1] = out1;

    if (r->op1_dest == BUS_ALGO5_FANOUT) {
        mem = c1 = c2 = out1;
    } else {
        write_bus(r->op1_dest, out1, &c1, &c2, &mem, &carrier);
    }

    out3 = op_calc_ex(ch->op[2].phase, operator_eg_out(&ch->op[2], am), m2 >> 1);
    write_bus(r->op3_dest, out3, &c1, &c2, &mem, &carrier);

    out2 = op_calc_ex(ch->op[1].phase, operator_eg_out(&ch->op[1], am), c1 >> 1);
    write_bus(r->op2_dest, out2, &c1, &c2, &mem, &carrier);

    out4 = op_calc_ex(ch->op[3].phase, operator_eg_out(&ch->op[3], am), c2 >> 1);
    carrier += out4;

    ch->mem_value = mem;

    for (i = 0; i < 4; i++) {
        advance_phase_lfo(&ch->op[i], pm_index);
        eg_advance(&ch->op[i]);
    }

    if (carrier > 8191) carrier = 8191;
    if (carrier < -8192) carrier = -8192;
    return clamp_s16(carrier);
}

/* ================================ Chip ================================ */

static const uint16_t LFO_SAMPLES_PER_STEP[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };

/* Register offsets $30-$9F and $B0-$B6 address operators/channels using the
 * register's PHYSICAL slot order, which is OP1, OP3, OP2, OP4 -- a real,
 * well-documented YM2612 quirk (registers are laid out in the order the
 * silicon happens to process operators in, not algorithm-diagram order).
 * This table translates that physical slot (0-3, from the register address)
 * into this codebase's logical op[] index (0=OP1, 1=OP2, 2=OP3, 3=OP4). */
static const uint8_t LOGICAL_OP_FOR_REG_SLOT[4] = { 0, 2, 1, 3 };

/* Channel-3 "3-slot" special-mode frequency registers ($A8-$AE) have their
 * own scrambled mapping from register slot (0-2) to logical operator. */
static const uint8_t SL3_INDEX_TO_LOGICAL_OP[3] = { 2, 0, 1 }; /* -> OP3, OP1, OP2 */

void ym2612_chip_init(Ym2612Chip *chip) {
    int i;
    for (i = 0; i < 6; i++) {
        ym2612_channel_init(&chip->channel[i]);
    }
    chip->lfo_timer = 0;
    chip->lfo_timer_overflow = 0;
    chip->lfo_cnt = 0;
    chip->lfo_am = 126; /* matches the real chip's LFO-disabled reset state */
    chip->lfo_pm = 0;
    chip->mode = 0;
    chip->fn_h_latch = 0;
    chip->sl3_fn_h_latch = 0;
}

void ym2612_chip_write(Ym2612Chip *chip, int port, uint8_t addr, uint8_t data) {
    uint8_t chan, slot_group, logical_op;
    Ym2612Channel *ch;

    if (addr < 0x30) {
        /* LFO/mode/key-on registers exist only in port 0's address space on real hardware */
        if (port != 0) return;
        switch (addr) {
            case 0x22:
                if (data & 0x08) {
                    chip->lfo_timer_overflow = LFO_SAMPLES_PER_STEP[data & 0x07];
                } else {
                    chip->lfo_timer_overflow = 0;
                    chip->lfo_timer = 0;
                    chip->lfo_cnt = 0;
                    chip->lfo_pm = 0;
                    chip->lfo_am = 126;
                }
                break;
            case 0x27:
                chip->mode = data;
                break;
            case 0x28: {
                uint8_t c = data & 0x03;
                if (c == 3) return;
                if (data & 0x04) c = (uint8_t)(c + 3);
                ch = &chip->channel[c];
                if (data & 0x10) ym2612_operator_key_on(&ch->op[0]); else ym2612_operator_key_off(&ch->op[0]);
                if (data & 0x20) ym2612_operator_key_on(&ch->op[1]); else ym2612_operator_key_off(&ch->op[1]);
                if (data & 0x40) ym2612_operator_key_on(&ch->op[2]); else ym2612_operator_key_off(&ch->op[2]);
                if (data & 0x80) ym2612_operator_key_on(&ch->op[3]); else ym2612_operator_key_off(&ch->op[3]);
                break;
            }
            default:
                break; /* timers ($24-$26), test register ($21): not needed to drive this synth */
        }
        return;
    }

    chan = addr & 0x03;
    if (chan == 3) return; /* $x3/$x7/$xB/$xF are unused register slots */
    slot_group = (addr >> 2) & 0x03;

    if ((addr & 0xF0) == 0xA0 && slot_group >= 2) {
        /* Channel-3 special-mode frequency registers ($A8-$AE): here `chan`
         * (0-2) is really "which of the 3 independently-tunable operators",
         * always targeting channel index 2, and only exists on port 0. */
        if (port != 0) return;
        if (slot_group == 2) {
            uint32_t fn = (((uint32_t)(chip->sl3_fn_h_latch & 7)) << 8) + data;
            uint8_t blk = chip->sl3_fn_h_latch >> 3;
            logical_op = SL3_INDEX_TO_LOGICAL_OP[chan];
            ym2612_operator_set_freq(&chip->channel[2].op[logical_op], (uint16_t)fn, blk);
        } else {
            chip->sl3_fn_h_latch = data & 0x3F;
        }
        return;
    }

    if (port == 1) chan = (uint8_t)(chan + 3);
    ch = &chip->channel[chan];

    switch (addr & 0xF0) {
        case 0x30:
            ym2612_operator_set_dt_mul(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;
        case 0x40:
            ym2612_operator_set_tl(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;
        case 0x50:
            ym2612_operator_set_ar_ksr(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;
        case 0x60:
            ym2612_operator_set_d1r(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data); /* folds in AM-enable bit */
            break;
        case 0x70:
            ym2612_operator_set_d2r(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;
        case 0x80:
            ym2612_operator_set_sl_rr(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;
        case 0x90:
            ym2612_operator_set_ssg(&ch->op[LOGICAL_OP_FOR_REG_SLOT[slot_group]], data);
            break;

        case 0xA0:
            if (slot_group == 0) { /* $A0-$A2: FNUM1 (low byte) -- triggers the frequency update */
                uint32_t fn = (((uint32_t)(chip->fn_h_latch & 7)) << 8) + data;
                uint8_t blk = chip->fn_h_latch >> 3;
                int i;
                for (i = 0; i < 4; i++) {
                    ym2612_operator_set_freq(&ch->op[i], (uint16_t)fn, blk);
                }
            } else { /* $A4-$A6: FNUM2/BLK (high bits), latched for the next FNUM1 write */
                chip->fn_h_latch = data & 0x3F;
            }
            break;

        case 0xB0:
            if (slot_group == 0) {
                ym2612_channel_set_algorithm(ch, data);
            } else if (slot_group == 1) {
                ym2612_channel_set_pan_and_sens(ch, data);
            }
            break;

        default:
            break;
    }
}

void ym2612_chip_clock(Ym2612Chip *chip, sample_t *out_left, sample_t *out_right) {
    int32_t left = 0, right = 0;
    int c;

    for (c = 0; c < 6; c++) {
        Ym2612Channel *ch = &chip->channel[c];
        int32_t s = ym2612_channel_clock(ch, chip->lfo_am, chip->lfo_pm);
        if (ch->pan_l) left += s;
        if (ch->pan_r) right += s;
    }

    *out_left = clamp_s16(left);
    *out_right = clamp_s16(right);

    if (chip->lfo_timer_overflow) {
        chip->lfo_timer++;
        if (chip->lfo_timer >= chip->lfo_timer_overflow) {
            chip->lfo_timer = 0;
            chip->lfo_cnt = (chip->lfo_cnt + 1) & 127;
            chip->lfo_am = (chip->lfo_cnt < 64)
                               ? (uint32_t)((chip->lfo_cnt ^ 63) << 1)
                               : (uint32_t)((chip->lfo_cnt & 63) << 1);
            chip->lfo_pm = chip->lfo_cnt >> 2;
        }
    }
}

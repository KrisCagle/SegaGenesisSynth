#ifndef YM2612_H
#define YM2612_H

#include "chip_types.h"

/* YM2612 FM synthesizer -- milestone 2a scope: a single operator's phase
 * generator + envelope generator + log-domain sine/exp synthesis, driven
 * directly rather than through the full 6-channel/8-algorithm register map
 * (that's a later milestone). The math here is the real thing though: the
 * same phase-increment formula, envelope rate tables, and log-domain
 * sine/attenuation combine the real chip uses. */

#define YM2612_CLOCK_HZ 7670442.0 /* NTSC */
#define YM2612_SAMPLE_HZ (YM2612_CLOCK_HZ / 144.0) /* native FM sample rate, ~53267Hz */

typedef enum {
    YM_EG_OFF = 0,
    YM_EG_REL = 1,
    YM_EG_SUS = 2,
    YM_EG_DEC = 3,
    YM_EG_ATT = 4
} Ym2612EgState;

typedef struct {
    /* --- register-derived parameters (set via the ym2612_operator_set_*
     * functions, mirroring the real per-operator register fields) --- */
    uint16_t mul;          /* already x2 scaled (0 means x0.5, matching the real chip's MUL=0 quirk) */
    const int16_t *dt;     /* selected 32-entry detune row, indexed by key code */
    uint32_t tl;            /* total level, in the envelope's 0-1023 attenuation domain */
    uint32_t ar, d1r, d2r, rr; /* pre-expanded rates (0, or 32.. / 34.. offset already applied) */
    uint32_t sl;             /* sustain level, in the 0-1023 attenuation domain */
    uint8_t  ksr_shift;      /* key-scale rate shift: 3 - KSR register field */
    uint8_t  ssg;            /* SSG-EG control: bit3 enable, bit2 attack, bit1 alternate, bit0 hold */

    uint32_t fc_raw;         /* channel-level (fnum<<block)>>1, before per-operator detune */
    uint32_t block_fnum;     /* raw (block<<11)|fnum, kept for LFO phase-modulation math */
    uint8_t  kc;             /* key code (0-31), derived from fnum/block */
    uint32_t am_mask;         /* 0 or 0xFFFFFFFF; from the $60-$6F register's AM-enable bit */

    /* --- live state --- */
    uint32_t phase;
    uint32_t phase_inc;
    Ym2612EgState state;
    int32_t  volume;    /* current attenuation, 0 (loudest) - 1023 (silent); SSG-EG can push it briefly higher.
                          * Signed (not unsigned) is load-bearing: the attack curve's `~volume` step below
                          * relies on two's-complement signed arithmetic shift to decay smoothly toward 0. */
    uint32_t vol_out;   /* volume combined with tl (and SSG-EG inversion), what synthesis actually reads */
    uint8_t  ssgn;       /* current SSG-EG inversion flag (0 or 4) */
    uint8_t  key;        /* key on/off latch */

    uint8_t  eg_sh_ar, eg_sh_d1r, eg_sh_d2r, eg_sh_rr;   /* envelope counter shift per phase */
    uint16_t eg_sel_ar, eg_sel_d1r, eg_sel_d2r, eg_sel_rr; /* envelope increment-table row per phase */
    uint32_t eg_timer;   /* divides the FM sample rate by 3 for the envelope clock */
    uint32_t eg_cnt;      /* envelope generator's own counter, wraps 4096 -> 1 (matches real chip) */
} Ym2612Operator;

void ym2612_operator_init(Ym2612Operator *op);

void ym2612_operator_set_dt_mul(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_tl(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_ar_ksr(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_d1r(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_d2r(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_sl_rr(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_ssg(Ym2612Operator *op, uint8_t reg_value);
void ym2612_operator_set_freq(Ym2612Operator *op, uint16_t fnum, uint8_t block);

void ym2612_operator_key_on(Ym2612Operator *op);
void ym2612_operator_key_off(Ym2612Operator *op);

sample_t ym2612_operator_clock(Ym2612Operator *op); /* advance one native FM sample (~53267Hz), return output */

/* --- Milestone 2b: a full 4-operator channel, wired through one of the 8
 * real FM algorithms with operator-1 self-feedback. The per-operator
 * register fields are unchanged -- configure ch.op[0..3] with the same
 * ym2612_operator_set_* functions above, then set the algorithm/feedback
 * and key the operators on/off as a channel. */

typedef struct {
    Ym2612Operator op[4]; /* OP1..OP4, matching standard FM algorithm-diagram numbering */
    uint8_t algo;          /* 0-7 */
    uint8_t fb_shift;      /* derived from the FB register field; 10 means "no feedback" */
    int32_t op1_out_hist[2]; /* OP1's last two raw outputs, for self-feedback */
    int32_t mem_value;        /* one-sample-delayed MEM bus value (used by algorithms 0-3, 5) */

    uint32_t pms;  /* PM sensitivity * 32 (index component into the LFO PM table); from $B4-$B6 */
    uint8_t  ams;  /* AM sensitivity, pre-converted to a shift amount; from $B4-$B6 */
    uint8_t  pan_l, pan_r; /* stereo output enable for this channel; from $B4-$B6 bits 7/6.
                             * Real hardware quirk, preserved here: both 0 means the channel is
                             * silent on BOTH speakers, not centered -- this is a hard on/off
                             * routing switch, not a continuous pan control. */
} Ym2612Channel;

void ym2612_channel_init(Ym2612Channel *ch);

/* Raw $B0-style register byte: bits 0-2 = algorithm, bits 3-5 = feedback. */
void ym2612_channel_set_algorithm(Ym2612Channel *ch, uint8_t reg_value);

/* Raw $B4-style register byte: bits 0-2 = PMS, bits 4-5 = AMS, bit7 = L enable, bit6 = R enable. */
void ym2612_channel_set_pan_and_sens(Ym2612Channel *ch, uint8_t reg_value);

/* op_bits: bit0=OP1 .. bit3=OP4, matching the real KEY ON/OFF register's
 * per-operator bits (shifted down to 0-3 here since this is one channel). */
void ym2612_channel_key_on(Ym2612Channel *ch, uint8_t op_bits);
void ym2612_channel_key_off(Ym2612Channel *ch, uint8_t op_bits);

/* lfo_am: current global LFO AM output (0 if LFO off/unused). lfo_pm: current
 * global LFO PM step (0 if LFO off/unused) -- combined internally with this
 * channel's own `pms`. Existing callers that don't use the LFO just pass 0, 0. */
sample_t ym2612_channel_clock(Ym2612Channel *ch, uint32_t lfo_am, uint32_t lfo_pm);

/* --- Milestone 2c: the full 6-channel chip, driven through its real
 * register map exactly like a game would (two 8-bit address/data port
 * pairs), plus the global LFO and channel-3's "3-slot" special mode where
 * 3 of its 4 operators can each run at an independent frequency. ---- */

typedef struct {
    Ym2612Channel channel[6];

    uint32_t lfo_timer;
    uint32_t lfo_timer_overflow; /* 0 = LFO held in reset (disabled) */
    uint32_t lfo_cnt;
    uint32_t lfo_am;  /* current AM output, 0-126 */
    uint32_t lfo_pm;   /* current PM step, 0-31 */

    uint8_t  mode;           /* $27: bits 6-7 enable channel-3's 3-slot special mode */
    uint8_t  fn_h_latch;      /* shared high-fnum/block latch for normal ($A4-$A6) freq writes */
    uint8_t  sl3_fn_h_latch;  /* separate latch for 3-slot-mode ($A8-$AE) freq writes */
} Ym2612Chip;

void ym2612_chip_init(Ym2612Chip *chip);

/* port: 0 or 1 (the real chip's two address/data port pairs -- port 0 addresses
 * channels 1-3, port 1 addresses channels 4-6, using identical register offsets). */
void ym2612_chip_write(Ym2612Chip *chip, int port, uint8_t addr, uint8_t data);

/* advance one native FM sample; mixes all 6 channels into separate left/right
 * outputs, honoring each channel's pan bits (milestone 2d). */
void ym2612_chip_clock(Ym2612Chip *chip, sample_t *out_left, sample_t *out_right);

#endif /* YM2612_H */

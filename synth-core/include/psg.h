#ifndef PSG_H
#define PSG_H

#include "chip_types.h"

/* SN76489 PSG emulation (3 tone channels + 1 noise channel), as used
 * alongside the YM2612 in the Genesis/Mega Drive. Platform-independent:
 * no I/O, no timing source of its own — the caller clocks it one internal
 * tick at a time via psg_clock(). */

#define PSG_CLOCK_HZ 3579545.0 /* NTSC SN76489 master clock */
#define PSG_TICK_HZ (PSG_CLOCK_HZ / 16.0) /* internal tick rate: clock / 16 */

typedef struct {
    uint16_t tone_reg[3];    /* 10-bit frequency registers, channels 0-2 */
    int16_t  tone_counter[3];/* down-counters, reload from tone_reg */
    uint8_t  tone_output[3]; /* current flip-flop state (0 or 1) per channel */

    uint8_t  noise_control;  /* bit2 = FB (0=periodic,1=white), bits1:0 = rate */
    int16_t  noise_counter;  /* down-counter for fixed noise rates */
    uint16_t lfsr;           /* 16-bit linear feedback shift register */

    uint8_t  volume[4];      /* 4-bit attenuation, one per channel incl. noise */

    uint8_t  latched_channel; /* 0-3, last latched channel */
    uint8_t  latched_type;    /* 0 = tone/noise control, 1 = volume */
} Psg;

void psg_reset(Psg *psg);
void psg_write(Psg *psg, uint8_t value);
sample_t psg_clock(Psg *psg); /* advance one internal tick, return mixed sample */

#endif /* PSG_H */

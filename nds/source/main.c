/* Nintendo DS port (Milestone 3): proves synth-core actually runs on real
 * DS-class hardware/emulator, the same "audible proof before UI" step
 * Milestone 1 did for the PC PSG port. No touchscreen UI yet -- that's a
 * later pass, likely translating a lot of the desktop UI's layout thinking
 * (sliders, algorithm diagram) onto the DS's second screen.
 *
 * Audio architecture: Maxmod's custom stream API (mmStreamOpen), opened on
 * the ARM9 with a fill-callback, is the same shape as the AudioStreamCallback
 * used on PC (desktop/main.c, pc/main.c) -- decimate each chip's native tick
 * rate into a requested interleaved 16-bit stereo buffer. No custom ARM7
 * firmware needed: devkitPro's calico environment supplies a default ARM7
 * binary that already handles Maxmod's low-level audio hardware access
 * (confirmed by reading the currently-installed nds-examples streaming
 * example and its Makefile, not assumed from memory -- libnds had a recent
 * breaking rewrite (v2.0 "calico") that removed the older FIFO-based API
 * most tutorials describe).
 *
 * synth-core/ itself is completely unchanged -- this file only calls its
 * existing public register-write/clock API, same as every other port.
 */

#include <nds.h>
#include <stdio.h>
#include <maxmod9.h>

#include "psg.h"
#include "ym2612.h"

#define OUTPUT_SAMPLE_RATE 32000

static Ym2612Chip g_chip;
static Psg g_psg;
static double g_fm_tick_accum = 0.0;
static double g_psg_tick_accum = 0.0;
static const double FM_TICKS_PER_SAMPLE = YM2612_SAMPLE_HZ / OUTPUT_SAMPLE_RATE;
static const double PSG_TICKS_PER_SAMPLE = PSG_TICK_HZ / OUTPUT_SAMPLE_RATE;

/* ---- Register-write helpers (same real protocol as every other port) ---- */

static uint16_t ym_fnum_for_hz(double hz, int block) {
    double fnum = hz * 1048576.0 / (YM2612_SAMPLE_HZ * (double)(1 << (block - 1)));
    if (fnum < 0.0) fnum = 0.0;
    if (fnum > 2047.0) fnum = 2047.0;
    return (uint16_t)(fnum + 0.5);
}

static void write_chip_freq(Ym2612Chip *chip, int port, int chan, double freq_hz, int block) {
    uint16_t fnum = ym_fnum_for_hz(freq_hz, block);
    ym2612_chip_write(chip, port, (uint8_t)(0xA4 + chan), (uint8_t)((block << 3) | ((fnum >> 8) & 7)));
    ym2612_chip_write(chip, port, (uint8_t)(0xA0 + chan), (uint8_t)(fnum & 0xFF));
}

static void chip_key(Ym2612Chip *chip, int global_chan, uint8_t op_on_bits) {
    uint8_t data = (uint8_t)((global_chan % 3) |
                              (global_chan >= 3 ? 0x04 : 0x00) |
                              (op_on_bits << 4));
    ym2612_chip_write(chip, 0, 0x28, data);
}

static const int LOGICAL_OP_REG_OFFSET[4] = { 0, 8, 4, 12 }; /* OP1,OP3,OP2,OP4 physical order */

typedef struct { int mul, dt, tl, ar, d1r, sl, rr; } Op;

static void write_patch(Ym2612Chip *chip, int port, int chan, int algo, int fb, const Op *ops) {
    int i;
    ym2612_chip_write(chip, port, (uint8_t)(0xB0 + chan), (uint8_t)((algo & 7) | ((fb & 7) << 3)));
    for (i = 0; i < 4; i++) {
        int off = LOGICAL_OP_REG_OFFSET[i];
        const Op *o = &ops[i];
        ym2612_chip_write(chip, port, (uint8_t)(0x30 + off + chan), (uint8_t)(((o->dt & 7) << 4) | (o->mul & 0x0F)));
        ym2612_chip_write(chip, port, (uint8_t)(0x40 + off + chan), (uint8_t)(o->tl & 0x7F));
        ym2612_chip_write(chip, port, (uint8_t)(0x50 + off + chan), (uint8_t)(o->ar & 0x1F));
        ym2612_chip_write(chip, port, (uint8_t)(0x60 + off + chan), (uint8_t)(o->d1r & 0x1F));
        ym2612_chip_write(chip, port, (uint8_t)(0x70 + off + chan), 0x00);
        ym2612_chip_write(chip, port, (uint8_t)(0x80 + off + chan), (uint8_t)(((o->sl & 0x0F) << 4) | (o->rr & 0x0F)));
        ym2612_chip_write(chip, port, (uint8_t)(0x90 + off + chan), 0x00);
    }
    ym2612_chip_write(chip, port, (uint8_t)(0xB4 + chan), 0xC0); /* pan both, no LFO sens */
}

static void psg_set_volume(Psg *psg, int channel, uint8_t attenuation) {
    psg_write(psg, (uint8_t)(0x80 | (channel << 5) | 0x10 | (attenuation & 0x0F)));
}

static void psg_set_noise(Psg *psg, uint8_t control) {
    psg_write(psg, (uint8_t)(0x80 | (3 << 5) | (control & 0x07)));
}

/* Same "E.PIANO" 2-pair (algorithm 4) patch already confirmed to sound good
 * on the desktop UI (desktop/main.c's preset_epiano), reused verbatim so we
 * know the *sound* isn't the variable being tested here -- only the platform. */
static const Op EPIANO_OPS[4] = {
    { 1, 4, 8, 31, 8, 3, 7 },
    { 1, 4, 2, 27, 6, 3, 6 },
    { 2, 4, 16, 31, 12, 3, 8 },
    { 1, 4, 10, 27, 8, 3, 7 }
};

/* ---- Audio stream fill callback (runs on ARM9, per Maxmod's docs) ---- */

static mm_word on_stream_request(mm_word length, mm_addr dest, mm_stream_formats format) {
    int16_t *out = (int16_t *)dest;
    mm_word len = length;
    (void)format;

    for (; len; len--) {
        int32_t left_sum = 0, right_sum = 0, psg_sum = 0;
        int fm_n, psg_n, k;

        g_fm_tick_accum += FM_TICKS_PER_SAMPLE;
        fm_n = (int)g_fm_tick_accum;
        if (fm_n < 1) fm_n = 1;
        g_fm_tick_accum -= fm_n;
        for (k = 0; k < fm_n; k++) {
            sample_t l, r;
            ym2612_chip_clock(&g_chip, &l, &r);
            left_sum += l;
            right_sum += r;
        }

        g_psg_tick_accum += PSG_TICKS_PER_SAMPLE;
        psg_n = (int)g_psg_tick_accum;
        if (psg_n < 1) psg_n = 1;
        g_psg_tick_accum -= psg_n;
        for (k = 0; k < psg_n; k++) {
            psg_sum += psg_clock(&g_psg);
        }

        {
            int16_t psg_avg = (int16_t)(psg_sum / psg_n);
            *out++ = clamp_s16(left_sum / fm_n + psg_avg);
            *out++ = clamp_s16(right_sum / fm_n + psg_avg);
        }
    }

    return length;
}

/* ---- Simple frame-counted test sequence: one FM chord note, then a PSG
 * noise blip -- deliberately modest (not exercising all 6 FM channels at
 * once) since this pass is about proving the pipeline works at all, not
 * stress-testing ARM9 CPU headroom yet. ---- */

int main(void) {
    unsigned int frame = 0;
    int fm_on = 0, fm_off = 0, noise_on = 0, noise_off = 0;

    consoleDemoInit();
    iprintf("\n  Genesis Synth -- NDS test\n\n");
    iprintf("  FM chord (E.PIANO patch)\n");
    iprintf("  then a PSG noise blip\n");

    ym2612_chip_init(&g_chip);
    psg_reset(&g_psg);

    write_patch(&g_chip, 0, 0, 4, 0, EPIANO_OPS);
    write_chip_freq(&g_chip, 0, 0, 440.0, 4); /* A4 */

    {
        mm_ds_system sys;
        sys.mod_count = 0;
        sys.samp_count = 0;
        sys.mem_bank = 0;
        mmInit(&sys);
    }

    {
        mm_stream stream;
        stream.sampling_rate = OUTPUT_SAMPLE_RATE;
        stream.buffer_length = 1200;
        stream.callback = on_stream_request;
        stream.format = MM_STREAM_16BIT_STEREO;
        stream.timer = MM_TIMER0;
        stream.manual = true;
        mmStreamOpen(&stream);
    }

    lcdSetVCountCompare(true, 0);
    irqEnable(IRQ_VCOUNT);

    while (pmMainLoop()) {
        swiIntrWait(1, IRQ_VCOUNT);
        mmStreamUpdate();
        swiWaitForVBlank();

        scanKeys();
        if (keysDown() & KEY_START) break;

        /* ~60 frames/sec: chord at 0, off+release at 2s, noise blip at 3s,
         * silence at 3.5s, loop the whole thing every 4s. */
        if (!fm_on && frame >= 0) { chip_key(&g_chip, 0, 0x0F); fm_on = 1; }
        if (!fm_off && frame >= 120) { chip_key(&g_chip, 0, 0x00); fm_off = 1; }
        if (!noise_on && frame >= 180) {
            psg_set_noise(&g_psg, 0x05); /* white noise, rate 1 */
            psg_set_volume(&g_psg, 3, 4);
            noise_on = 1;
        }
        if (!noise_off && frame >= 210) { psg_set_volume(&g_psg, 3, 15); noise_off = 1; }

        frame++;
        if (frame >= 240) {
            frame = 0;
            fm_on = fm_off = noise_on = noise_off = 0;
        }
    }

    return 0;
}

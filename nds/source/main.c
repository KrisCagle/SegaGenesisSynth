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
#include <math.h>
#include <maxmod9.h>

#include "psg.h"
#include "ym2612.h"

/* Matches the sampling_rate used by the proven-clean devkitPro reference
 * example (audio/maxmod/streaming) rather than an arbitrary round number --
 * confirmed clean on Kris's actual hardware, whereas our own 32000Hz build
 * was buzzing/crackling specifically while a note played. The DS timer
 * hardware can't hit every requested rate exactly; a rate that doesn't
 * divide cleanly can end up running at a slightly different real rate than
 * requested, which would desync our fixed-point tick ratios (computed
 * assuming the requested rate is exact) from what's actually being played
 * back -- reusing a rate already confirmed to work sidesteps that. */
#define OUTPUT_SAMPLE_RATE 25000

/* The DS's ARM9 (ARM946E-S) has no hardware FPU, so `double` math in the
 * per-sample audio path gets emulated in slow software float -- fine on PC,
 * but on real hardware it was eating enough of the per-sample budget to
 * cause audible crackle (buffer underrun). The tick-rate ratios themselves
 * are computed once at compile time from a constant double expression (the
 * compiler folds this into a plain integer, no runtime float cost); only
 * the per-sample accumulation is fixed-point from here on, same "no floats
 * in the real-time path" rule chip_types.h already applies to synth-core
 * itself, just extended to this platform's glue code too. */
#define TICK_FX_BITS 16

static Ym2612Chip g_chip;
static Psg g_psg;
static uint32_t g_fm_tick_accum_fx = 0;
static uint32_t g_psg_tick_accum_fx = 0;
static const uint32_t FM_TICKS_PER_SAMPLE_FX =
    (uint32_t)(YM2612_SAMPLE_HZ * (double)(1u << TICK_FX_BITS) / OUTPUT_SAMPLE_RATE + 0.5);
static const uint32_t PSG_TICKS_PER_SAMPLE_FX =
    (uint32_t)(PSG_TICK_HZ * (double)(1u << TICK_FX_BITS) / OUTPUT_SAMPLE_RATE + 0.5);

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

/* The DS UI only ever plays one voice (single touch point -> monophonic
 * piano, one channel's worth of algorithm/operator editing), but
 * ym2612_chip_clock() unconditionally processes all 6 channels (24
 * operators) every single call, mixing in 5 channels that are permanently
 * silent here. That's up to 6x more log-domain sine/envelope work than this
 * build needs, on a 67MHz chip with no FPU -- clocking channel 0 directly
 * (both the struct field and ym2612_channel_clock are public, exactly for
 * cases like this) cuts real per-sample CPU cost by roughly 5/6, which is
 * the actual budget this build was missing, not just the float-vs-fixed-point
 * issue from the crackle fix. LFO is never enabled by this UI (no register
 * $22 writes), so passing 0,0 for lfo_am/lfo_pm matches what chip_clock
 * would compute anyway. */
static mm_word on_stream_request(mm_word length, mm_addr dest, mm_stream_formats format) {
    int16_t *out = (int16_t *)dest;
    mm_word len = length;
    (void)format;

    for (; len; len--) {
        int32_t left_sum = 0, right_sum = 0, psg_sum = 0;
        int fm_n, psg_n, k;

        g_fm_tick_accum_fx += FM_TICKS_PER_SAMPLE_FX;
        fm_n = (int)(g_fm_tick_accum_fx >> TICK_FX_BITS);
        if (fm_n < 1) fm_n = 1;
        g_fm_tick_accum_fx -= (uint32_t)fm_n << TICK_FX_BITS;
        for (k = 0; k < fm_n; k++) {
            sample_t s = ym2612_channel_clock(&g_chip.channel[0], 0, 0);
            left_sum += s;
            right_sum += s;
        }

        g_psg_tick_accum_fx += PSG_TICKS_PER_SAMPLE_FX;
        psg_n = (int)(g_psg_tick_accum_fx >> TICK_FX_BITS);
        if (psg_n < 1) psg_n = 1;
        g_psg_tick_accum_fx -= (uint32_t)psg_n << TICK_FX_BITS;
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

/* ---- Touch UI ----
 *
 * Two 256x192 screens, one touch point. Graphics approach confirmed against
 * the locally-installed examples before writing any of this:
 *   - Bottom (touch) screen: main engine, MODE_5_2D, a BgType_Bmp16 layer
 *     (raw RGB15 pixel writes -- the DS equivalent of raylib's DrawRectangle,
 *     confirmed against Graphics/Backgrounds/{all_in_one,Double_Buffer}).
 *   - Top screen: sub engine text console (the same consoleDemoInit() API
 *     Milestone 3 already proved), showing page/operator/value readouts.
 *     Double_Buffer's example puts its bitmap on main and its console on
 *     sub as two *separate* screens rather than compositing text over the
 *     bitmap on one screen -- that combined-overlay trick was the original
 *     plan but isn't confirmed anywhere in the installed examples, so this
 *     follows the pattern that IS confirmed: values live on the top screen,
 *     the bottom screen is pure graphical touch surface.
 *   - lcdMainOnBottom() swaps physical screens so the bitmap (main engine)
 *     ends up on the physical bottom/touch screen, matching where a player's
 *     thumb actually is.
 *   - touchRead()/KEY_TOUCH confirmed against input/Touch_Pad/touch_test.
 *
 * Single touch point means single-voice: the whole UI drives one FM channel
 * (global channel 0), same simplification the piano row implies -- no
 * chords, matching what a one-finger touchscreen can actually express.
 */

#define SCREEN_W 256
#define SCREEN_H 192

static u16 *g_fb; /* bitmap framebuffer for the touch (bottom) screen */

static u16 mkcolor(int r, int g, int b) { return (u16)(RGB15(r, g, b) | BIT(15)); }

static void fill_rect(int x0, int y0, int x1, int y1, u16 color) {
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            g_fb[y * SCREEN_W + x] = color;
}

typedef struct { int x0, y0, x1, y1; } Rect;

static int rect_hit(Rect r, int px, int py) {
    return px >= r.x0 && px <= r.x1 && py >= r.y0 && py <= r.y1;
}

/* Draws a horizontal slider track + filled portion + a bright handle line,
 * matching the desktop app's slider look (dim track, bright fill-to-value). */
static void draw_slider(Rect r, int value, int min, int max) {
    int span = r.x1 - r.x0;
    int fill_x = r.x0 + (int)((long)(value - min) * span / (max - min));
    fill_rect(r.x0, r.y0, r.x1, r.y1, mkcolor(6, 6, 9));
    fill_rect(r.x0, r.y0, fill_x, r.y1, mkcolor(10, 18, 28));
    fill_rect(fill_x - 1, r.y0 - 2, fill_x + 1, r.y1 + 2, mkcolor(31, 31, 20));
}

static int slider_value_from_x(Rect r, int px, int min, int max) {
    int span = r.x1 - r.x0;
    if (px < r.x0) px = r.x0;
    if (px > r.x1) px = r.x1;
    return min + (int)((long)(px - r.x0) * (max - min) / span);
}

static void draw_button(Rect r, int selected, u16 base_color) {
    fill_rect(r.x0, r.y0, r.x1, r.y1, selected ? mkcolor(31, 31, 20) : base_color);
}

/* ---- Page 1 (play): algorithm picker + feedback slider + touch piano ---- */

static const Rect ALGO_BTN[8] = {
    {2, 4, 29, 24}, {32, 4, 59, 24}, {62, 4, 89, 24}, {92, 4, 119, 24},
    {122, 4, 149, 24}, {152, 4, 179, 24}, {182, 4, 209, 24}, {212, 4, 253, 24}
};
static const Rect FEEDBACK_SLIDER = { 4, 40, 251, 56 };
static const Rect PIANO_ROW = { 0, 136, 255, 191 };
#define PIANO_KEYS 13

static double g_piano_freq[PIANO_KEYS];
static int g_piano_key = -1; /* -1 = no note held; single touch point */

static void piano_init_freqs(void) {
    int i;
    for (i = 0; i < PIANO_KEYS; i++)
        g_piano_freq[i] = 261.625565 * pow(2.0, i / 12.0); /* C4..C5 chromatic */
}

static int piano_key_from_x(int px) {
    int key = (px - PIANO_ROW.x0) * PIANO_KEYS / (PIANO_ROW.x1 - PIANO_ROW.x0 + 1);
    if (key < 0) key = 0;
    if (key > PIANO_KEYS - 1) key = PIANO_KEYS - 1;
    return key;
}

static void draw_page1(int algo, int fb) {
    int i;
    for (i = 0; i < 8; i++)
        draw_button(ALGO_BTN[i], i == algo, mkcolor(5, 10, 16));
    draw_slider(FEEDBACK_SLIDER, fb, 0, 7);
    for (i = 0; i < PIANO_KEYS; i++) {
        int x0 = PIANO_ROW.x0 + i * (PIANO_ROW.x1 - PIANO_ROW.x0 + 1) / PIANO_KEYS;
        int x1 = PIANO_ROW.x0 + (i + 1) * (PIANO_ROW.x1 - PIANO_ROW.x0 + 1) / PIANO_KEYS - 2;
        fill_rect(x0, PIANO_ROW.y0, x1, PIANO_ROW.y1,
                  i == g_piano_key ? mkcolor(31, 31, 20) : mkcolor(20, 20, 24));
    }
}

/* ---- Page 2 (edit): one operator's 6 core sliders, L/R cycles operator --- */

static const int OP_SLIDER_MIN[6] = { 0, 0, 0, 0, 0, 0 };
static const int OP_SLIDER_MAX[6] = { 15, 127, 31, 31, 15, 15 };

static Rect op_slider_rect(int i) {
    Rect r;
    r.x0 = 4; r.x1 = 251;
    r.y0 = 8 + i * 28;
    r.y1 = r.y0 + 14;
    return r;
}

static int *op_slider_field(Op *op, int i) {
    switch (i) {
        case 0: return &op->mul;
        case 1: return &op->tl;
        case 2: return &op->ar;
        case 3: return &op->d1r;
        case 4: return &op->sl;
        default: return &op->rr;
    }
}

static void draw_page2(const Op *op) {
    int i;
    for (i = 0; i < 6; i++)
        draw_slider(op_slider_rect(i), *op_slider_field((Op *)op, i), OP_SLIDER_MIN[i], OP_SLIDER_MAX[i]);
}

/* ---- Main ---- */

static Op g_ops[4];
static int g_algo = 4, g_feedback = 0;
static int g_page = 0;      /* 0 = play, 1 = edit */
static int g_cur_op = 0;    /* which operator page 2 is editing */
static int g_drag = -1;     /* -1 none, 0 = feedback, 1 = piano, 10+i = op slider i */

/* Redrawing (bitmap + console text) is gated behind this flag so idle frames
 * (nothing touched) skip all drawing work entirely -- the graphics/text cost
 * was competing with on_stream_request for ARM9 cycles every single frame,
 * even when the screen had nothing new to show, which is what turned the
 * already-known crackle into much worse distortion once this UI pass added
 * real per-frame drawing work. Set to 1 whenever any on-screen state changes. */
static int g_dirty = 1;

/* Gating idle frames wasn't enough on its own: the moment that matters most
 * -- pressing/dragging the piano -- is exactly the moment g_dirty keeps
 * getting set, so during active play the full-screen clear + redraw +
 * console reprint (all VRAM writes, which have real access-time overhead
 * beyond plain RAM) was still landing on every single frame, right when
 * audio timing is most sensitive. Throttling actual redraws to once every
 * few frames caps how often that expensive work can compete with
 * mmStreamUpdate(), while still redrawing the latest state promptly enough
 * to look responsive (a slider lagging a couple frames behind a finger is
 * invisible; audio glitching on every touch is not). */
static int g_frame_counter = 0;
#define REDRAW_THROTTLE_FRAMES 4

static void apply_patch(void) {
    write_patch(&g_chip, 0, 0, g_algo, g_feedback, g_ops);
}

int main(void) {
    int i;
    for (i = 0; i < 4; i++) g_ops[i] = EPIANO_OPS[i];
    piano_init_freqs();

    /* Two full 256x256x16bpp banks (128KB each) for a real double buffer --
     * draw into the hidden half while the other half is on screen, then flip
     * during vblank. A single buffer (the original approach) writes pixels
     * into the same VRAM the display is actively scanning, which is what was
     * causing the visible flashing/tearing. Pattern confirmed against the
     * installed Graphics/Backgrounds/Double_Buffer example. */
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_BG);
    int bg = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    g_fb = (u16 *)bgGetGfxPtr(bg) + 256 * 256; /* start drawing into the hidden half */

    consoleDemoInit();
    lcdMainOnBottom(); /* bitmap touch UI on bottom screen, console on top */

    ym2612_chip_init(&g_chip);
    psg_reset(&g_psg);
    apply_patch();

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

        scanKeys();
        int down = keysDown();
        int held = keysHeld();

        if (down & KEY_START) {
            g_page ^= 1;
            g_drag = -1;
            g_dirty = 1;
        }
        if (g_page == 1) {
            if (down & KEY_L) { g_cur_op = (g_cur_op + 3) % 4; g_drag = -1; g_dirty = 1; }
            if (down & KEY_R) { g_cur_op = (g_cur_op + 1) % 4; g_drag = -1; g_dirty = 1; }
        }

        touchPosition touch;
        touchRead(&touch);
        int touching = held & KEY_TOUCH;
        int px = touch.px, py = touch.py;

        if (g_page == 0) {
            if (down & KEY_TOUCH) {
                for (i = 0; i < 8; i++) {
                    if (rect_hit(ALGO_BTN[i], px, py)) { g_algo = i; apply_patch(); g_dirty = 1; }
                }
                if (rect_hit(FEEDBACK_SLIDER, px, py)) g_drag = 0;
                else if (rect_hit(PIANO_ROW, px, py)) g_drag = 1;
            }
            if (touching && g_drag == 0) {
                int new_fb = slider_value_from_x(FEEDBACK_SLIDER, px, 0, 7);
                if (new_fb != g_feedback) { g_feedback = new_fb; apply_patch(); g_dirty = 1; }
            }
            if (touching && g_drag == 1) {
                int key = piano_key_from_x(px);
                if (key != g_piano_key) {
                    if (g_piano_key < 0) {
                        write_chip_freq(&g_chip, 0, 0, g_piano_freq[key], 4);
                        chip_key(&g_chip, 0, 0x0F);
                    } else {
                        write_chip_freq(&g_chip, 0, 0, g_piano_freq[key], 4);
                    }
                    g_piano_key = key;
                    g_dirty = 1;
                }
            }
            if (!touching && g_drag >= 0) {
                if (g_drag == 1 && g_piano_key >= 0) {
                    chip_key(&g_chip, 0, 0x00);
                    g_piano_key = -1;
                    g_dirty = 1;
                }
                g_drag = -1;
            }
        } else {
            Op *op = &g_ops[g_cur_op];
            if (down & KEY_TOUCH) {
                for (i = 0; i < 6; i++)
                    if (rect_hit(op_slider_rect(i), px, py)) g_drag = 10 + i;
            }
            if (touching && g_drag >= 10) {
                int idx = g_drag - 10;
                int new_val = slider_value_from_x(op_slider_rect(idx), px, OP_SLIDER_MIN[idx], OP_SLIDER_MAX[idx]);
                int *field = op_slider_field(op, idx);
                if (new_val != *field) { *field = new_val; apply_patch(); g_dirty = 1; }
            }
            if (!touching) g_drag = -1;
        }

        mmStreamUpdate(); /* safety net around the drawing work below */

        g_frame_counter++;
        int should_redraw = g_dirty && (g_frame_counter % REDRAW_THROTTLE_FRAMES == 0);

        if (should_redraw) {
            fill_rect(0, 0, SCREEN_W - 1, SCREEN_H - 1, mkcolor(1, 1, 2));
            if (g_page == 0) {
                draw_page1(g_algo, g_feedback);
            } else {
                draw_page2(&g_ops[g_cur_op]);
            }

            consoleClear();
            iprintf("\n  Genesis Synth -- NDS\n\n");
            if (g_page == 0) {
                iprintf("  PAGE 1: PLAY\n");
                iprintf("  Algorithm: %d   Feedback: %d\n\n", g_algo, g_feedback);
                iprintf("  Touch piano row to play.\n");
                iprintf("  START: edit page\n");
            } else {
                Op *op = &g_ops[g_cur_op];
                iprintf("  PAGE 2: EDIT  (L/R: op)\n");
                iprintf("  Operator %d of 4\n\n", g_cur_op + 1);
                iprintf("  MUL %2d   TL  %3d\n", op->mul, op->tl);
                iprintf("  AR  %2d   D1R %2d\n", op->ar, op->d1r);
                iprintf("  SL  %2d   RR  %2d\n\n", op->sl, op->rr);
                iprintf("  START: play page\n");
            }
        }

        swiWaitForVBlank();

        if (should_redraw) {
            /* Flip the bitmap double buffer: what we just drew becomes the
             * visible half, and the now-hidden half (the previous front
             * buffer) becomes the new draw target. */
            g_fb = (u16 *)bgGetGfxPtr(bg);
            if (bgGetMapBase(bg) == 8) bgSetMapBase(bg, 0);
            else bgSetMapBase(bg, 8);
            g_dirty = 0;
        }
    }

    return 0;
}

/* Playable raylib desktop app for synth-core's YM2612 emulation (UI v2).
 *
 * Drives the same public register-write API (ym2612_chip_write /
 * ym2612_chip_clock) that pc/main.c and a real game would use -- this app
 * only adds a window, controls, and a piano keyboard on top. synth-core
 * itself is untouched.
 *
 * Threading note: raylib's audio stream callback runs on its own thread
 * (raylib's audio module is miniaudio-based). g_chip is written from the
 * main thread (UI) and read/advanced from the audio thread with no lock --
 * a deliberate simplification common to small raylib audio-stream apps;
 * worst case is an occasional single-sample glitch, not a crash, since
 * every field involved is a plain integer read/write.
 */

#include <stdio.h>
#include <math.h>
#include "raylib.h"

#include "psg.h"
#include "ym2612.h"

#define SCREEN_W 1150
#define SCREEN_H 780
#define OUTPUT_SAMPLE_RATE 48000

/* ---- Audio engine ---- */

static Ym2612Chip g_chip;
static const double FM_TICKS_PER_SAMPLE = YM2612_SAMPLE_HZ / OUTPUT_SAMPLE_RATE;
static double g_fm_tick_accum = 0.0;

static void AudioStreamCallback(void *buffer_data, unsigned int frames) {
    int16_t *out = (int16_t *)buffer_data;
    unsigned int i;

    for (i = 0; i < frames; i++) {
        int32_t left_sum = 0, right_sum = 0;
        int fm_n, k;

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

        out[i * 2 + 0] = clamp_s16(left_sum / fm_n);
        out[i * 2 + 1] = clamp_s16(right_sum / fm_n);
    }
}

/* ---- Register-write helpers (same real protocol as pc/main.c) ---- */

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

/* op_on_bits: bit0=OP1 .. bit3=OP4 (0 = key everything off). global_chan: 0-5. */
static void chip_key(Ym2612Chip *chip, int global_chan, uint8_t op_on_bits) {
    uint8_t data = (uint8_t)((global_chan % 3) |
                              (global_chan >= 3 ? 0x04 : 0x00) |
                              (op_on_bits << 4));
    ym2612_chip_write(chip, 0, 0x28, data);
}

/* ---- Patch model: one algorithm/feedback + 4 operators + LFO sensitivity,
 * applied to all 6 physical channels so every voice shares the same
 * live-editable sound. ---- */

typedef struct {
    int mul;  /* 0-15 register field (0 = x0.5) */
    int dt;   /* 0-7 detune (4 = none; not exposed as a slider yet, presets use it) */
    int tl;   /* 0-127, 0 = loudest */
    int ar;   /* 0-31 */
    int d1r;  /* 0-31 */
    int sl;   /* 0-15 */
    int rr;   /* 0-15 */
} OperatorParams;

typedef struct {
    int algo;         /* 0-7 */
    int feedback;     /* 0-7 */
    int ams;          /* 0-3, AM sensitivity to the LFO */
    int pms;          /* 0-7, PM (vibrato) sensitivity to the LFO */
    int am_enable;    /* 0/1: whether operators respond to AM at all (simplified: all-or-nothing) */
    OperatorParams op[4]; /* OP1..OP4 */
} PatchParams;

static PatchParams default_patch(void) {
    PatchParams p;
    p.algo = 0;
    p.feedback = 0;
    p.ams = 0;
    p.pms = 0;
    p.am_enable = 0;
    p.op[0] = (OperatorParams){ 1, 4, 22, 31, 10, 4, 8 };  /* OP1: modulator */
    p.op[1] = (OperatorParams){ 1, 4, 26, 31, 10, 4, 8 };  /* OP2: modulator */
    p.op[2] = (OperatorParams){ 1, 4, 30, 31, 10, 4, 8 };  /* OP3: modulator */
    p.op[3] = (OperatorParams){ 1, 4, 5, 31, 10, 4, 8 };   /* OP4: carrier, loud */
    return p;
}

/* A handful of starting points -- FM's parameter space is huge, so these
 * are hand-picked to land somewhere recognizable; tweak from here by ear. */
static PatchParams preset_epiano(void) {
    PatchParams p = { 0 };
    p.algo = 4; p.feedback = 0;
    p.op[0] = (OperatorParams){ 1, 4, 8, 31, 8, 3, 7 };
    p.op[1] = (OperatorParams){ 1, 4, 2, 27, 6, 3, 6 };
    p.op[2] = (OperatorParams){ 2, 4, 16, 31, 12, 3, 8 };
    p.op[3] = (OperatorParams){ 1, 4, 10, 27, 8, 3, 7 };
    return p;
}
static PatchParams preset_bass(void) {
    PatchParams p = { 0 };
    p.algo = 0; p.feedback = 3;
    p.op[0] = (OperatorParams){ 1, 4, 28, 31, 14, 6, 10 };
    p.op[1] = (OperatorParams){ 2, 4, 32, 31, 14, 6, 10 };
    p.op[2] = (OperatorParams){ 1, 4, 20, 31, 10, 4, 9 };
    p.op[3] = (OperatorParams){ 1, 4, 4, 31, 8, 2, 9 };
    return p;
}
static PatchParams preset_bell(void) {
    PatchParams p = { 0 };
    p.algo = 5; p.feedback = 0;
    p.op[0] = (OperatorParams){ 1, 7, 8, 31, 6, 2, 6 };
    p.op[1] = (OperatorParams){ 1, 4, 6, 31, 4, 1, 5 };
    p.op[2] = (OperatorParams){ 2, 4, 14, 31, 6, 2, 6 };
    p.op[3] = (OperatorParams){ 3, 4, 20, 31, 8, 3, 7 };
    return p;
}
static PatchParams preset_brass(void) {
    PatchParams p = { 0 };
    p.algo = 4; p.feedback = 2;
    p.op[0] = (OperatorParams){ 1, 4, 18, 25, 10, 4, 9 };
    p.op[1] = (OperatorParams){ 1, 4, 6, 22, 8, 3, 8 };
    p.op[2] = (OperatorParams){ 1, 4, 22, 25, 10, 4, 9 };
    p.op[3] = (OperatorParams){ 1, 4, 8, 22, 8, 3, 8 };
    return p;
}
static PatchParams preset_lead(void) {
    PatchParams p = { 0 };
    p.algo = 2; p.feedback = 4;
    p.op[0] = (OperatorParams){ 3, 4, 26, 31, 12, 5, 9 };
    p.op[1] = (OperatorParams){ 1, 4, 20, 31, 10, 4, 8 };
    p.op[2] = (OperatorParams){ 1, 4, 14, 31, 10, 4, 8 };
    p.op[3] = (OperatorParams){ 1, 4, 6, 31, 8, 3, 8 };
    return p;
}

typedef struct {
    const char *name;
    PatchParams (*make)(void);
} PresetDef;

static const PresetDef PRESETS[5] = {
    { "E.PIANO", preset_epiano },
    { "BASS", preset_bass },
    { "BELL", preset_bell },
    { "BRASS", preset_brass },
    { "LEAD", preset_lead }
};

/* Register layout quirk (same as pc/main.c): physical slot offsets are
 * OP1=+0, OP3=+4, OP2=+8, OP4=+12 -- indexed here by logical op (0=OP1..3=OP4). */
static const int LOGICAL_OP_REG_OFFSET[4] = { 0, 8, 4, 12 };

static void apply_patch_to_channel(Ym2612Chip *chip, int port, int chan, const PatchParams *p) {
    int op;
    ym2612_chip_write(chip, port, (uint8_t)(0xB0 + chan), (uint8_t)((p->algo & 7) | ((p->feedback & 7) << 3)));
    for (op = 0; op < 4; op++) {
        int off = LOGICAL_OP_REG_OFFSET[op];
        const OperatorParams *o = &p->op[op];
        uint8_t d1r_byte = (uint8_t)((o->d1r & 0x1F) | (p->am_enable ? 0x80 : 0x00));
        ym2612_chip_write(chip, port, (uint8_t)(0x30 + off + chan), (uint8_t)(((o->dt & 7) << 4) | (o->mul & 0x0F)));
        ym2612_chip_write(chip, port, (uint8_t)(0x40 + off + chan), (uint8_t)(o->tl & 0x7F));
        ym2612_chip_write(chip, port, (uint8_t)(0x50 + off + chan), (uint8_t)(o->ar & 0x1F));  /* KS=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x60 + off + chan), d1r_byte);
        ym2612_chip_write(chip, port, (uint8_t)(0x70 + off + chan), 0x00);                      /* D2R=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x80 + off + chan), (uint8_t)(((o->sl & 0x0F) << 4) | (o->rr & 0x0F)));
        ym2612_chip_write(chip, port, (uint8_t)(0x90 + off + chan), 0x00);                      /* SSG-EG off */
    }
    /* pan: both L+R on, plus this patch's AMS/PMS */
    ym2612_chip_write(chip, port, (uint8_t)(0xB4 + chan),
                       (uint8_t)(0xC0 | ((p->ams & 3) << 4) | (p->pms & 7)));
}

static void apply_patch_all(Ym2612Chip *chip, const PatchParams *p) {
    int c;
    for (c = 0; c < 3; c++) apply_patch_to_channel(chip, 0, c, p);
    for (c = 0; c < 3; c++) apply_patch_to_channel(chip, 1, c, p);
}

static void apply_lfo(Ym2612Chip *chip, int enabled, int rate) {
    ym2612_chip_write(chip, 0, 0x22, (uint8_t)((enabled ? 0x08 : 0x00) | (rate & 7)));
}

/* ---- Voice allocation: each held note claims one of the 6 physical
 * channels, round-robin, so up to 6-note chords work. ---- */

typedef struct {
    int active;
    int note_id;
} Voice;

static Voice g_voices[6];

static void note_on(Ym2612Chip *chip, int note_id, double freq_hz) {
    int i;
    for (i = 0; i < 6; i++) {
        if (!g_voices[i].active) {
            write_chip_freq(chip, i / 3, i % 3, freq_hz, 4);
            chip_key(chip, i, 0x0F);
            g_voices[i].active = 1;
            g_voices[i].note_id = note_id;
            return;
        }
    }
    /* all 6 voices busy: v1 just drops the note rather than stealing one */
}

static void note_off(Ym2612Chip *chip, int note_id) {
    int i;
    for (i = 0; i < 6; i++) {
        if (g_voices[i].active && g_voices[i].note_id == note_id) {
            chip_key(chip, i, 0x00);
            g_voices[i].active = 0;
        }
    }
}

/* ---- Piano keyboard: one octave, mouse-clickable + tracker-style QWERTY,
 * shiftable up/down across a few extra octaves. ---- */

typedef struct {
    int vkey;
    double base_freq_hz; /* at octave shift 0 */
    int is_black;
    float wx; /* position in "white key width" units */
    const char *label;
} PianoKeyDef;

static const PianoKeyDef PIANO_KEYS[13] = {
    { KEY_Z,     261.63, 0, 0.0f, "C4" },
    { KEY_S,     277.18, 1, 0.5f, "" },
    { KEY_X,     293.66, 0, 1.0f, "D4" },
    { KEY_D,     311.13, 1, 1.5f, "" },
    { KEY_C,     329.63, 0, 2.0f, "E4" },
    { KEY_V,     349.23, 0, 3.0f, "F4" },
    { KEY_G,     369.99, 1, 3.5f, "" },
    { KEY_B,     392.00, 0, 4.0f, "G4" },
    { KEY_H,     415.30, 1, 4.5f, "" },
    { KEY_N,     440.00, 0, 5.0f, "A4" },
    { KEY_J,     466.16, 1, 5.5f, "" },
    { KEY_M,     493.88, 0, 6.0f, "B4" },
    { KEY_COMMA, 523.25, 0, 7.0f, "C5" }
};

#define PIANO_X 140
#define PIANO_Y 555
#define WHITE_KEY_W 60
#define WHITE_KEY_H 140
#define BLACK_KEY_W 36
#define BLACK_KEY_H 90
#define OCTAVE_MIN -2
#define OCTAVE_MAX 2

static int g_key_down[13] = { 0 };
static int g_octave = 0;

static Rectangle piano_key_rect(int i) {
    const PianoKeyDef *k = &PIANO_KEYS[i];
    if (!k->is_black) {
        return (Rectangle){ PIANO_X + k->wx * WHITE_KEY_W, PIANO_Y, WHITE_KEY_W - 2, WHITE_KEY_H };
    }
    return (Rectangle){ PIANO_X + k->wx * WHITE_KEY_W - BLACK_KEY_W / 2.0f, PIANO_Y, BLACK_KEY_W, BLACK_KEY_H };
}

static int mouse_hit_key(Vector2 mouse) {
    int i;
    for (i = 0; i < 13; i++) {
        if (PIANO_KEYS[i].is_black && CheckCollisionPointRec(mouse, piano_key_rect(i))) return i;
    }
    for (i = 0; i < 13; i++) {
        if (!PIANO_KEYS[i].is_black && CheckCollisionPointRec(mouse, piano_key_rect(i))) return i;
    }
    return -1;
}

static void update_piano(void) {
    int i;
    int hit = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? mouse_hit_key(GetMousePosition()) : -1;
    double octave_mult = pow(2.0, (double)g_octave);

    for (i = 0; i < 13; i++) {
        int want = IsKeyDown(PIANO_KEYS[i].vkey) || (hit == i);
        if (want && !g_key_down[i]) {
            note_on(&g_chip, i, PIANO_KEYS[i].base_freq_hz * octave_mult);
        } else if (!want && g_key_down[i]) {
            note_off(&g_chip, i);
        }
        g_key_down[i] = want;
    }
}

static void draw_piano(void) {
    int i;
    for (i = 0; i < 13; i++) {
        if (!PIANO_KEYS[i].is_black) {
            Color c = g_key_down[i] ? (Color){ 120, 190, 255, 255 } : RAYWHITE;
            DrawRectangleRec(piano_key_rect(i), c);
            DrawRectangleLinesEx(piano_key_rect(i), 1, (Color){ 40, 40, 45, 255 });
            if (PIANO_KEYS[i].label[0]) {
                Rectangle r = piano_key_rect(i);
                DrawText(PIANO_KEYS[i].label, (int)(r.x + 6), (int)(r.y + r.height - 20), 12, (Color){ 60, 60, 65, 255 });
            }
        }
    }
    for (i = 0; i < 13; i++) {
        if (PIANO_KEYS[i].is_black) {
            Color c = g_key_down[i] ? (Color){ 80, 150, 220, 255 } : (Color){ 20, 20, 24, 255 };
            DrawRectangleRec(piano_key_rect(i), c);
        }
    }
}

/* ---- Minimal UI widgets ---- */

static int Slider(Rectangle bounds, const char *label, int *value, int min_v, int max_v) {
    Vector2 mouse = GetMousePosition();
    int changed = 0;
    int hot = CheckCollisionPointRec(mouse, bounds);
    float t;
    char valtext[16];

    if (hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        float nt = (mouse.x - bounds.x) / bounds.width;
        int newval;
        if (nt < 0.0f) nt = 0.0f;
        if (nt > 1.0f) nt = 1.0f;
        newval = min_v + (int)(nt * (float)(max_v - min_v) + 0.5f);
        if (newval != *value) {
            *value = newval;
            changed = 1;
        }
    }

    DrawRectangleRec(bounds, (Color){ 40, 40, 48, 255 });
    t = (float)(*value - min_v) / (float)(max_v - min_v);
    DrawRectangle((int)bounds.x, (int)bounds.y, (int)(bounds.width * t), (int)bounds.height, (Color){ 90, 170, 250, 255 });
    DrawRectangleLinesEx(bounds, 1, (Color){ 80, 80, 90, 255 });
    DrawText(label, (int)bounds.x, (int)bounds.y - 15, 12, RAYWHITE);
    snprintf(valtext, sizeof valtext, "%d", *value);
    DrawText(valtext, (int)(bounds.x + bounds.width + 8), (int)(bounds.y + 2), 12, RAYWHITE);
    return changed;
}

static int Button(Rectangle bounds, const char *label, int selected) {
    Vector2 mouse = GetMousePosition();
    int hot = CheckCollisionPointRec(mouse, bounds);
    Color col = selected ? (Color){ 90, 170, 250, 255 } : (hot ? (Color){ 65, 65, 78, 255 } : (Color){ 42, 42, 50, 255 });
    int text_w = MeasureText(label, 13);

    DrawRectangleRec(bounds, col);
    DrawRectangleLinesEx(bounds, 1, (Color){ 80, 80, 90, 255 });
    DrawText(label, (int)(bounds.x + bounds.width / 2 - text_w / 2), (int)(bounds.y + bounds.height / 2 - 7), 13, RAYWHITE);

    return (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ? 1 : 0;
}

static int Toggle(Rectangle bounds, const char *label, int *value) {
    if (Button(bounds, label, *value)) {
        *value = !*value;
        return 1;
    }
    return 0;
}

/* ---- Algorithm routing diagram --------------------------------------
 * Simplified but faithful "who feeds whom" view: OP1-4 in a fixed vertical
 * stack, arrows to whichever operator(s)/OUT they feed for the selected
 * algorithm. (Algorithms 0-3 route through a one-sample-delayed internal
 * bus in the real chip; drawn here as a direct arrow for clarity.) */

typedef struct { int from; int to; } AlgoEdge; /* 0-3 = OP1-4, 4 = OUT */

static const AlgoEdge ALGO_EDGES[8][6] = {
    /* 0 */ { {0,1},{1,2},{2,3},{3,4},{-1,-1},{-1,-1} },
    /* 1 */ { {0,2},{1,2},{2,3},{3,4},{-1,-1},{-1,-1} },
    /* 2 */ { {0,3},{1,2},{2,3},{3,4},{-1,-1},{-1,-1} },
    /* 3 */ { {0,1},{1,3},{2,3},{3,4},{-1,-1},{-1,-1} },
    /* 4 */ { {0,1},{1,4},{2,3},{3,4},{-1,-1},{-1,-1} },
    /* 5 */ { {0,1},{0,2},{0,3},{1,4},{2,4},{3,4} },
    /* 6 */ { {0,1},{1,4},{2,4},{3,4},{-1,-1},{-1,-1} },
    /* 7 */ { {0,4},{1,4},{2,4},{3,4},{-1,-1},{-1,-1} }
};

static void draw_algo_diagram(Rectangle area, int algo) {
    Vector2 node_pos[5];
    const char *node_label[5] = { "1", "2", "3", "4", "OUT" };
    int i;
    const float box_w = 32, box_h = 26;

    DrawRectangleLinesEx(area, 1, (Color){ 60, 60, 70, 255 });

    node_pos[0] = (Vector2){ area.x + 16, area.y + 10 };
    node_pos[1] = (Vector2){ area.x + 16, area.y + 48 };
    node_pos[2] = (Vector2){ area.x + 16, area.y + 86 };
    node_pos[3] = (Vector2){ area.x + 16, area.y + 124 };
    node_pos[4] = (Vector2){ area.x + area.width - 46, area.y + 71 };

    for (i = 0; i < 6; i++) {
        AlgoEdge e = ALGO_EDGES[algo][i];
        Vector2 a, b, dir;
        float len;
        if (e.from < 0) continue;
        a = (Vector2){ node_pos[e.from].x + box_w, node_pos[e.from].y + box_h / 2 };
        b = (Vector2){ node_pos[e.to].x, node_pos[e.to].y + box_h / 2 };
        DrawLineEx(a, b, 1.5f, (Color){ 90, 170, 250, 255 });
        dir = (Vector2){ b.x - a.x, b.y - a.y };
        len = sqrtf(dir.x * dir.x + dir.y * dir.y);
        if (len > 0.1f) {
            Vector2 tip = b;
            Vector2 ux = { dir.x / len, dir.y / len };
            Vector2 uy = { -ux.y, ux.x };
            Vector2 p1 = { tip.x - ux.x * 7 + uy.x * 4, tip.y - ux.y * 7 + uy.y * 4 };
            Vector2 p2 = { tip.x - ux.x * 7 - uy.x * 4, tip.y - ux.y * 7 - uy.y * 4 };
            DrawTriangle(tip, p1, p2, (Color){ 90, 170, 250, 255 });
        }
    }

    for (i = 0; i < 5; i++) {
        Rectangle r = { node_pos[i].x, node_pos[i].y, box_w, box_h };
        Color fill = (i == 4) ? (Color){ 70, 130, 90, 255 } : (Color){ 70, 70, 85, 255 };
        int tw = MeasureText(node_label[i], 14);
        DrawRectangleRec(r, fill);
        DrawRectangleLinesEx(r, 2, (Color){ 140, 140, 155, 255 });
        DrawText(node_label[i], (int)(r.x + box_w / 2 - tw / 2), (int)(r.y + box_h / 2 - 7), 14, RAYWHITE);
    }
}

/* ---- Envelope graph: simulate the real envelope generator for a
 * representative key-on/hold/key-off cycle and plot it, so the shape
 * you see is the shape you actually get -- not a stylized approximation. */

#define ENV_GRAPH_POINTS 100
#define ENV_GRAPH_STRIDE 300
#define ENV_GRAPH_RELEASE_AT 70

static void compute_envelope_graph(const OperatorParams *p, float *out) {
    Ym2612Operator op;
    int i, k;

    ym2612_operator_init(&op);
    ym2612_operator_set_dt_mul(&op, (uint8_t)(((p->dt & 7) << 4) | (p->mul & 0x0F)));
    ym2612_operator_set_tl(&op, (uint8_t)p->tl);
    ym2612_operator_set_ar_ksr(&op, (uint8_t)p->ar);
    ym2612_operator_set_d1r(&op, (uint8_t)p->d1r);
    ym2612_operator_set_d2r(&op, 0);
    ym2612_operator_set_sl_rr(&op, (uint8_t)(((p->sl & 0x0F) << 4) | (p->rr & 0x0F)));
    ym2612_operator_set_freq(&op, 700, 4);
    ym2612_operator_key_on(&op);

    for (i = 0; i < ENV_GRAPH_POINTS; i++) {
        if (i == ENV_GRAPH_RELEASE_AT) ym2612_operator_key_off(&op);
        for (k = 0; k < ENV_GRAPH_STRIDE; k++) ym2612_operator_clock(&op);
        {
            int32_t v = (int32_t)op.vol_out;
            if (v < 0) v = 0;
            if (v > 1023) v = 1023;
            out[i] = 1.0f - (float)v / 1023.0f;
        }
    }
}

static void draw_envelope_graph(Rectangle area, const OperatorParams *p) {
    float values[ENV_GRAPH_POINTS];
    int i;

    compute_envelope_graph(p, values);

    DrawRectangleRec(area, (Color){ 18, 18, 22, 255 });
    DrawRectangleLinesEx(area, 1, (Color){ 60, 60, 70, 255 });

    for (i = 0; i < ENV_GRAPH_POINTS - 1; i++) {
        Vector2 a = { area.x + area.width * ((float)i / (ENV_GRAPH_POINTS - 1)), area.y + area.height * (1.0f - values[i]) };
        Vector2 b = { area.x + area.width * ((float)(i + 1) / (ENV_GRAPH_POINTS - 1)), area.y + area.height * (1.0f - values[i + 1]) };
        DrawLineEx(a, b, 1.5f, (Color){ 120, 220, 160, 255 });
    }
}

/* ---- Main ---- */

int main(void) {
    PatchParams patch = default_patch();
    AudioStream stream;
    int lfo_enabled = 0;
    int lfo_rate = 3;
    int op;

    InitWindow(SCREEN_W, SCREEN_H, "Genesis FM Synth");
    SetTargetFPS(60);

    InitAudioDevice();
    ym2612_chip_init(&g_chip);
    stream = LoadAudioStream(OUTPUT_SAMPLE_RATE, 16, 2);
    SetAudioStreamCallback(stream, AudioStreamCallback);
    PlayAudioStream(stream);

    apply_patch_all(&g_chip, &patch);
    apply_lfo(&g_chip, lfo_enabled, lfo_rate);

    while (!WindowShouldClose()) {
        int patch_changed = 0;
        int lfo_changed = 0;
        int i;

        update_piano();

        if (IsKeyPressed(KEY_LEFT_BRACKET) && g_octave > OCTAVE_MIN) g_octave--;
        if (IsKeyPressed(KEY_RIGHT_BRACKET) && g_octave < OCTAVE_MAX) g_octave++;

        /* Algorithm + feedback */
        for (i = 0; i < 8; i++) {
            Rectangle b = (Rectangle){ 40.0f + i * 46, 60, 40, 34 };
            if (Button(b, TextFormat("%d", i), patch.algo == i)) {
                patch.algo = i;
                patch_changed = 1;
            }
        }
        patch_changed |= Slider((Rectangle){ 40, 145, 180, 12 }, "FEEDBACK", &patch.feedback, 0, 7);

        draw_algo_diagram((Rectangle){ 420, 55, 220, 160 }, patch.algo);

        /* LFO controls */
        lfo_changed |= Toggle((Rectangle){ 660, 55, 70, 28 }, "LFO", &lfo_enabled);
        lfo_changed |= Slider((Rectangle){ 660, 105, 130, 12 }, "LFO RATE", &lfo_rate, 0, 7);
        patch_changed |= Slider((Rectangle){ 660, 145, 130, 12 }, "PMS (vibrato)", &patch.pms, 0, 7);
        patch_changed |= Slider((Rectangle){ 830, 145, 130, 12 }, "AMS (tremolo)", &patch.ams, 0, 3);
        patch_changed |= Toggle((Rectangle){ 830, 55, 130, 28 }, "AM ENABLE", &patch.am_enable);

        /* Presets */
        for (i = 0; i < 5; i++) {
            Rectangle b = (Rectangle){ 660.0f + i * 96, 178, 90, 22 };
            if (Button(b, PRESETS[i].name, 0)) {
                patch = PRESETS[i].make();
                patch_changed = 1;
            }
        }
        DrawText("PRESETS", 660, 165, 11, (Color){ 160, 160, 170, 255 });

        /* Operator panels */
        for (op = 0; op < 4; op++) {
            float px = 40.0f + op * 260.0f;
            float py = 250.0f;
            char title[8];
            snprintf(title, sizeof title, "OP%d", op + 1);
            DrawText(title, (int)px, (int)(py - 25), 18, (Color){ 90, 170, 250, 255 });

            patch_changed |= Slider((Rectangle){ px, py + 10, 200, 12 }, "MUL (harmonic ratio)", &patch.op[op].mul, 0, 15);
            patch_changed |= Slider((Rectangle){ px, py + 50, 200, 12 }, "TL (0=loudest)", &patch.op[op].tl, 0, 127);
            patch_changed |= Slider((Rectangle){ px, py + 90, 200, 12 }, "AR (attack speed)", &patch.op[op].ar, 0, 31);
            patch_changed |= Slider((Rectangle){ px, py + 130, 200, 12 }, "D1R (decay speed)", &patch.op[op].d1r, 0, 31);
            patch_changed |= Slider((Rectangle){ px, py + 170, 200, 12 }, "SL (sustain level)", &patch.op[op].sl, 0, 15);
            patch_changed |= Slider((Rectangle){ px, py + 210, 200, 12 }, "RR (release speed)", &patch.op[op].rr, 0, 15);

            draw_envelope_graph((Rectangle){ px, py + 235, 200, 50 }, &patch.op[op]);
        }

        if (patch_changed) apply_patch_all(&g_chip, &patch);
        if (lfo_changed) apply_lfo(&g_chip, lfo_enabled, lfo_rate);

        /* Octave controls, next to the piano */
        {
            int oct_down_clicked = Button((Rectangle){ 40, PIANO_Y, 40, 34 }, "OCT-", 0);
            int oct_up_clicked = Button((Rectangle){ 40, PIANO_Y + 44, 40, 34 }, "OCT+", 0);
            if (oct_down_clicked && g_octave > OCTAVE_MIN) g_octave--;
            if (oct_up_clicked && g_octave < OCTAVE_MAX) g_octave++;
            DrawText(TextFormat("Octave %+d", g_octave), 40, PIANO_Y + 86, 14, (Color){ 200, 200, 210, 255 });
        }

        BeginDrawing();
        ClearBackground((Color){ 24, 24, 28, 255 });
        DrawText("Genesis FM Synth", 40, 15, 24, RAYWHITE);
        draw_piano();
        DrawText("Play: mouse, or Z S X D C V G B H N J M , -- octave: [ ]", PIANO_X, PIANO_Y + WHITE_KEY_H + 15, 14, (Color){ 160, 160, 170, 255 });
        EndDrawing();
    }

    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

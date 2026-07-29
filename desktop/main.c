/* Playable raylib desktop app for synth-core's YM2612 emulation (UI v1).
 *
 * Drives the same public register-write API (ym2612_chip_write /
 * ym2612_chip_clock) that pc/main.c and a real game would use -- this app
 * only adds a window, sliders, and a piano keyboard on top. synth-core
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

#define SCREEN_W 1000
#define SCREEN_H 650
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

/* ---- Patch model: one algorithm/feedback + 4 operators, applied to all 6
 * physical channels so every voice shares the same live-editable sound. ---- */

typedef struct {
    int mul;  /* 0-15 register field (0 = x0.5) */
    int tl;   /* 0-127, 0 = loudest */
    int ar;   /* 0-31 */
    int d1r;  /* 0-31 */
    int sl;   /* 0-15 */
    int rr;   /* 0-15 */
} OperatorParams;

typedef struct {
    int algo;         /* 0-7 */
    int feedback;     /* 0-7 */
    OperatorParams op[4]; /* OP1..OP4 */
} PatchParams;

static PatchParams default_patch(void) {
    PatchParams p;
    p.algo = 0;
    p.feedback = 0;
    p.op[0] = (OperatorParams){ 1, 22, 31, 10, 4, 8 };  /* OP1: modulator */
    p.op[1] = (OperatorParams){ 1, 26, 31, 10, 4, 8 };  /* OP2: modulator */
    p.op[2] = (OperatorParams){ 1, 30, 31, 10, 4, 8 };  /* OP3: modulator */
    p.op[3] = (OperatorParams){ 1, 5, 31, 10, 4, 8 };   /* OP4: carrier, loud */
    return p;
}

/* Register layout quirk (same as pc/main.c): physical slot offsets are
 * OP1=+0, OP3=+4, OP2=+8, OP4=+12 -- indexed here by logical op (0=OP1..3=OP4). */
static const int LOGICAL_OP_REG_OFFSET[4] = { 0, 8, 4, 12 };

static void apply_patch_to_channel(Ym2612Chip *chip, int port, int chan, const PatchParams *p) {
    int op;
    ym2612_chip_write(chip, port, (uint8_t)(0xB0 + chan), (uint8_t)((p->algo & 7) | ((p->feedback & 7) << 3)));
    for (op = 0; op < 4; op++) {
        int off = LOGICAL_OP_REG_OFFSET[op];
        const OperatorParams *o = &p->op[op];
        ym2612_chip_write(chip, port, (uint8_t)(0x30 + off + chan), (uint8_t)(o->mul & 0x0F)); /* DT=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x40 + off + chan), (uint8_t)(o->tl & 0x7F));
        ym2612_chip_write(chip, port, (uint8_t)(0x50 + off + chan), (uint8_t)(o->ar & 0x1F));  /* KS=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x60 + off + chan), (uint8_t)(o->d1r & 0x1F)); /* AM=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x70 + off + chan), 0x00);                      /* D2R=0 */
        ym2612_chip_write(chip, port, (uint8_t)(0x80 + off + chan), (uint8_t)(((o->sl & 0x0F) << 4) | (o->rr & 0x0F)));
        ym2612_chip_write(chip, port, (uint8_t)(0x90 + off + chan), 0x00);                      /* SSG-EG off */
    }
    ym2612_chip_write(chip, port, (uint8_t)(0xB4 + chan), 0xC0); /* pan: both L+R on, no LFO sens */
}

static void apply_patch_all(Ym2612Chip *chip, const PatchParams *p) {
    int c;
    for (c = 0; c < 3; c++) apply_patch_to_channel(chip, 0, c, p);
    for (c = 0; c < 3; c++) apply_patch_to_channel(chip, 1, c, p);
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

/* ---- Piano keyboard: one octave, mouse-clickable + tracker-style QWERTY ---- */

typedef struct {
    int vkey;
    double freq_hz;
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

#define PIANO_X 40
#define PIANO_Y 470
#define WHITE_KEY_W 60
#define WHITE_KEY_H 140
#define BLACK_KEY_W 36
#define BLACK_KEY_H 90

static int g_key_down[13] = { 0 };

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

    for (i = 0; i < 13; i++) {
        int want = IsKeyDown(PIANO_KEYS[i].vkey) || (hit == i);
        if (want && !g_key_down[i]) {
            note_on(&g_chip, i, PIANO_KEYS[i].freq_hz);
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
    float t;
    char valtext[16];

    if (CheckCollisionPointRec(mouse, bounds) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
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

static int AlgoButton(Rectangle bounds, int index, int selected) {
    Vector2 mouse = GetMousePosition();
    int hot = CheckCollisionPointRec(mouse, bounds);
    Color col = (selected == index) ? (Color){ 90, 170, 250, 255 } : (hot ? (Color){ 65, 65, 78, 255 } : (Color){ 42, 42, 50, 255 });
    char label[4];

    DrawRectangleRec(bounds, col);
    DrawRectangleLinesEx(bounds, 1, (Color){ 80, 80, 90, 255 });
    snprintf(label, sizeof label, "%d", index);
    DrawText(label, (int)(bounds.x + bounds.width / 2 - 4), (int)(bounds.y + bounds.height / 2 - 6), 14, RAYWHITE);

    return (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ? 1 : 0;
}

/* ---- Main ---- */

int main(void) {
    PatchParams patch = default_patch();
    AudioStream stream;
    int op;

    InitWindow(SCREEN_W, SCREEN_H, "Genesis FM Synth");
    SetTargetFPS(60);

    InitAudioDevice();
    ym2612_chip_init(&g_chip);
    stream = LoadAudioStream(OUTPUT_SAMPLE_RATE, 16, 2);
    SetAudioStreamCallback(stream, AudioStreamCallback);
    PlayAudioStream(stream);

    apply_patch_all(&g_chip, &patch);

    while (!WindowShouldClose()) {
        int changed = 0;
        int i;

        update_piano();

        for (i = 0; i < 8; i++) {
            Rectangle b = (Rectangle){ 40.0f + i * 46, 55, 40, 34 };
            if (AlgoButton(b, i, patch.algo)) {
                patch.algo = i;
                changed = 1;
            }
        }
        changed |= Slider((Rectangle){ 470, 68, 140, 14 }, "FEEDBACK", &patch.feedback, 0, 7);

        for (op = 0; op < 4; op++) {
            float px = 40.0f + op * 235.0f;
            float py = 150.0f;
            char title[8];
            snprintf(title, sizeof title, "OP%d", op + 1);
            DrawText(title, (int)px, (int)(py - 25), 18, (Color){ 90, 170, 250, 255 });

            changed |= Slider((Rectangle){ px, py + 10, 180, 12 }, "MUL", &patch.op[op].mul, 0, 15);
            changed |= Slider((Rectangle){ px, py + 50, 180, 12 }, "TL (0=loud)", &patch.op[op].tl, 0, 127);
            changed |= Slider((Rectangle){ px, py + 90, 180, 12 }, "AR", &patch.op[op].ar, 0, 31);
            changed |= Slider((Rectangle){ px, py + 130, 180, 12 }, "D1R", &patch.op[op].d1r, 0, 31);
            changed |= Slider((Rectangle){ px, py + 170, 180, 12 }, "SL", &patch.op[op].sl, 0, 15);
            changed |= Slider((Rectangle){ px, py + 210, 180, 12 }, "RR", &patch.op[op].rr, 0, 15);
        }

        if (changed) {
            apply_patch_all(&g_chip, &patch);
        }

        BeginDrawing();
        ClearBackground((Color){ 24, 24, 28, 255 });
        DrawText("Genesis FM Synth", 40, 15, 24, RAYWHITE);
        DrawText("Algorithm", 40, 40, 14, (Color){ 160, 160, 170, 255 });
        draw_piano();
        DrawText("Play: mouse, or Z S X D C V G B H N J M ,", PIANO_X, PIANO_Y + WHITE_KEY_H + 15, 14, (Color){ 160, 160, 170, 255 });
        EndDrawing();
    }

    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

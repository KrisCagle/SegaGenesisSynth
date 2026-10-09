/* Playable raylib desktop app for Genisys (UI v3, on the shared engine).
 *
 * All sound comes from engine/ (genisys_engine.h), the same code the plugin
 * uses. This file only adds a window, controls, a piano keyboard, MIDI
 * input and WAV recording.
 *
 * Threading: the engine is single-threaded by contract and lives on
 * raylib's audio thread. The UI thread and the Windows MIDI thread never
 * touch it; each sends events through its own lock-free single-producer /
 * single-consumer queue, which the audio callback drains before rendering.
 * (Previously the UI wrote chip registers while the audio thread was
 * reading them.)
 *
 * Builds with GCC/MinGW (raylib from MSYS2); uses C11 <stdatomic.h>.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "raylib.h"

#include "genisys_engine.h"
#include "genisys_presets.h"
#include "midi_input.h" /* no windows.h in this file -- see midi_input.h for why */

#define SCREEN_W 1150
#define SCREEN_H 880
#define OUTPUT_SAMPLE_RATE 48000

/* ---- UI/MIDI -> audio thread event queues ---- */

typedef enum { EV_NOTE_ON, EV_NOTE_OFF, EV_PATCH, EV_PSG } EventType;

typedef struct {
    EventType type;
    int note;
    int velocity;
    GenisysPatch patch;
    GenisysPsgSettings psg;
} Event;

#define QUEUE_LEN 256

typedef struct {
    Event items[QUEUE_LEN];
    atomic_uint head; /* next slot to read: written only by the consumer (audio thread) */
    atomic_uint tail; /* next slot to write: written only by the producer */
} EventQueue;

/* Returns 0 (and drops the event) if the queue is full. */
static int queue_push(EventQueue *q, const Event *ev) {
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned next = (t + 1) % QUEUE_LEN;
    if (next == atomic_load_explicit(&q->head, memory_order_acquire)) return 0;
    q->items[t] = *ev;
    atomic_store_explicit(&q->tail, next, memory_order_release); /* publish the item */
    return 1;
}

static int queue_pop(EventQueue *q, Event *ev) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (h == atomic_load_explicit(&q->tail, memory_order_acquire)) return 0;
    *ev = q->items[h];
    atomic_store_explicit(&q->head, (h + 1) % QUEUE_LEN, memory_order_release);
    return 1;
}

static EventQueue g_ui_queue;   /* producer: UI thread */
static EventQueue g_midi_queue; /* producer: Windows MIDI thread */

static void send_note(EventQueue *q, EventType type, int note, int velocity) {
    Event ev = { 0 };
    ev.type = type;
    ev.note = note;
    ev.velocity = velocity;
    queue_push(q, &ev);
}

/* ---- WAV recording (captures the final mixed output) ---- */

#define RECORD_MAX_FRAMES ((size_t)OUTPUT_SAMPLE_RATE * 60 * 5) /* 5 minutes, stereo */

static int16_t *g_record_buffer = NULL;
static size_t g_record_frames = 0;
static int g_recording = 0;

static void record_push(int16_t left, int16_t right) {
    if (!g_recording || !g_record_buffer || g_record_frames >= RECORD_MAX_FRAMES) return;
    g_record_buffer[g_record_frames * 2 + 0] = left;
    g_record_buffer[g_record_frames * 2 + 1] = right;
    g_record_frames++;
}

/* ---- Audio thread ---- */

static GenisysEngine g_engine; /* after startup, touched only by the audio callback */

static void apply_event(const Event *ev) {
    switch (ev->type) {
        case EV_NOTE_ON:  genisys_engine_note_on(&g_engine, ev->note, ev->velocity); break;
        case EV_NOTE_OFF: genisys_engine_note_off(&g_engine, ev->note); break;
        case EV_PATCH:    genisys_engine_set_patch(&g_engine, &ev->patch); break;
        case EV_PSG:      genisys_engine_set_psg(&g_engine, &ev->psg); break;
    }
}

#define RENDER_CHUNK 1024

static void AudioStreamCallback(void *buffer_data, unsigned int frames) {
    static float left[RENDER_CHUNK], right[RENDER_CHUNK];
    int16_t *out = (int16_t *)buffer_data;
    unsigned int done = 0, i;
    Event ev;

    while (queue_pop(&g_ui_queue, &ev)) apply_event(&ev);
    while (queue_pop(&g_midi_queue, &ev)) apply_event(&ev);

    while (done < frames) {
        unsigned int n = frames - done;
        if (n > RENDER_CHUNK) n = RENDER_CHUNK;
        genisys_engine_render(&g_engine, left, right, (int)n);
        for (i = 0; i < n; i++) {
            /* The device is 16-bit, so clipping happens here, at the very end,
             * rather than inside the chip mix. */
            int16_t l = clamp_s16((int32_t)lrintf(left[i] * 32768.0f));
            int16_t r = clamp_s16((int32_t)lrintf(right[i] * 32768.0f));
            out[(done + i) * 2 + 0] = l;
            out[(done + i) * 2 + 1] = r;
            record_push(l, r);
        }
        done += n;
    }
}

/* ---- Patch state (UI thread). Mute/solo/AM are UI conveniences folded
 * into the patch actually sent to the engine. ---- */

static GenisysPatch g_patch;
static int g_op_mute[4], g_op_solo[4];
static int g_am_enable = 0;
static GenisysPsgSettings g_psg_ui;

static GenisysPatch effective_patch(void) {
    GenisysPatch p = g_patch;
    int op, any_solo = 0;
    for (op = 0; op < 4; op++) if (g_op_solo[op]) any_solo = 1;
    for (op = 0; op < 4; op++) {
        int silenced = any_solo ? !g_op_solo[op] : g_op_mute[op];
        if (silenced) p.op[op].tl = 127;
        p.op[op].am = g_am_enable;
    }
    return p;
}

/* ---- Piano keyboard: one octave, mouse-clickable + tracker-style QWERTY,
 * shiftable up/down across a few extra octaves. ---- */

typedef struct {
    int vkey;
    int semitone; /* above C4 (MIDI 60), at octave shift 0 */
    int is_black;
    float wx; /* position in "white key width" units */
    const char *label;
} PianoKeyDef;

static const PianoKeyDef PIANO_KEYS[13] = {
    { KEY_Z,      0, 0, 0.0f, "C4" },
    { KEY_S,      1, 1, 0.5f, "" },
    { KEY_X,      2, 0, 1.0f, "D4" },
    { KEY_D,      3, 1, 1.5f, "" },
    { KEY_C,      4, 0, 2.0f, "E4" },
    { KEY_V,      5, 0, 3.0f, "F4" },
    { KEY_G,      6, 1, 3.5f, "" },
    { KEY_B,      7, 0, 4.0f, "G4" },
    { KEY_H,      8, 1, 4.5f, "" },
    { KEY_N,      9, 0, 5.0f, "A4" },
    { KEY_J,     10, 1, 5.5f, "" },
    { KEY_M,     11, 0, 6.0f, "B4" },
    { KEY_COMMA, 12, 0, 7.0f, "C5" }
};

#define PIANO_X 140
#define PIANO_Y 650
#define WHITE_KEY_W 60
#define WHITE_KEY_H 140
#define BLACK_KEY_W 36
#define BLACK_KEY_H 90
#define OCTAVE_MIN -2
#define OCTAVE_MAX 2
#define KEYBOARD_VELOCITY 100

/* MIDI note each on-screen key is currently holding, or -1. Remembering the
 * exact note means changing octave mid-hold still releases the right one. */
static int g_key_note[13];
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

    for (i = 0; i < 13; i++) {
        int want = IsKeyDown(PIANO_KEYS[i].vkey) || (hit == i);
        if (want && g_key_note[i] < 0) {
            int note = 60 + PIANO_KEYS[i].semitone + 12 * g_octave;
            send_note(&g_ui_queue, EV_NOTE_ON, note, KEYBOARD_VELOCITY);
            g_key_note[i] = note;
        } else if (!want && g_key_note[i] >= 0) {
            send_note(&g_ui_queue, EV_NOTE_OFF, g_key_note[i], 0);
            g_key_note[i] = -1;
        }
    }
}

static void draw_piano(void) {
    int i;
    for (i = 0; i < 13; i++) {
        if (!PIANO_KEYS[i].is_black) {
            Color c = g_key_note[i] >= 0 ? (Color){ 120, 190, 255, 255 } : RAYWHITE;
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
            Color c = g_key_note[i] >= 0 ? (Color){ 80, 150, 220, 255 } : (Color){ 20, 20, 24, 255 };
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

static void compute_envelope_graph(const GenisysOperatorParams *p, float *out) {
    Ym2612Operator op;
    int i, k;

    ym2612_operator_init(&op);
    ym2612_operator_set_dt_mul(&op, (uint8_t)(((p->dt & 7) << 4) | (p->mul & 0x0F)));
    ym2612_operator_set_tl(&op, (uint8_t)p->tl);
    ym2612_operator_set_ar_ksr(&op, (uint8_t)(((p->ks & 3) << 6) | (p->ar & 0x1F)));
    ym2612_operator_set_d1r(&op, (uint8_t)p->d1r);
    ym2612_operator_set_d2r(&op, (uint8_t)p->d2r);
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

static void draw_envelope_graph(Rectangle area, const GenisysOperatorParams *p) {
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

/* ---- MIDI glue: runs on the Windows MIDI thread, so it only queues. ---- */

static void midi_note_on(int note, int velocity) { send_note(&g_midi_queue, EV_NOTE_ON, note, velocity); }
static void midi_note_off(int note) { send_note(&g_midi_queue, EV_NOTE_OFF, note, 0); }

/* ---- Main ---- */

int main(void) {
    AudioStream stream;
    const char *midi_status;
    int record_index = 0;
    int patch_dirty = 0, psg_dirty = 0;
    int op, i;

    for (i = 0; i < 13; i++) g_key_note[i] = -1;
    g_patch = genisys_default_patch();
    /* The desktop app's single PSG LEVEL slider drives a unison layer. */
    g_psg_ui = genisys_default_psg();
    g_psg_ui.mode = GENISYS_PSG_UNISON;
    g_psg_ui.level = 0;

    InitWindow(SCREEN_W, SCREEN_H, "Genisys");
    SetTargetFPS(60);

    /* Set up the engine fully before the audio thread starts. */
    genisys_engine_global_init();
    genisys_engine_init(&g_engine, OUTPUT_SAMPLE_RATE);
    {
        GenisysPatch p = effective_patch();
        genisys_engine_set_patch(&g_engine, &p);
        genisys_engine_set_psg(&g_engine, &g_psg_ui);
    }

    InitAudioDevice();
    stream = LoadAudioStream(OUTPUT_SAMPLE_RATE, 16, 2);
    SetAudioStreamCallback(stream, AudioStreamCallback);
    PlayAudioStream(stream);

    g_record_buffer = (int16_t *)malloc(RECORD_MAX_FRAMES * 2 * sizeof(int16_t));

    midi_status = midi_input_init(midi_note_on, midi_note_off);

    while (!WindowShouldClose()) {
        int patch_changed = 0;
        int psg_changed = 0;

        /* raylib widgets draw as they handle input, so the frame has to be
         * open before any of them run. */
        BeginDrawing();
        ClearBackground((Color){ 24, 24, 28, 255 });
        DrawText("Genisys", 40, 15, 24, RAYWHITE);
        DrawText(midi_status, 700, 22, 14, (Color){ 160, 160, 170, 255 });

        update_piano();

        if (IsKeyPressed(KEY_LEFT_BRACKET) && g_octave > OCTAVE_MIN) g_octave--;
        if (IsKeyPressed(KEY_RIGHT_BRACKET) && g_octave < OCTAVE_MAX) g_octave++;

        /* Algorithm + feedback */
        for (i = 0; i < 8; i++) {
            Rectangle b = (Rectangle){ 40.0f + i * 46, 60, 40, 34 };
            if (Button(b, TextFormat("%d", i), g_patch.algorithm == i)) {
                g_patch.algorithm = i;
                patch_changed = 1;
            }
        }
        patch_changed |= Slider((Rectangle){ 40, 145, 180, 12 }, "FEEDBACK", &g_patch.feedback, 0, 7);
        patch_changed |= Slider((Rectangle){ 240, 145, 150, 12 }, "VELOCITY SENS %", &g_patch.velocity_sens, 0, 100);

        draw_algo_diagram((Rectangle){ 420, 55, 220, 160 }, g_patch.algorithm);

        /* LFO controls */
        patch_changed |= Toggle((Rectangle){ 660, 55, 70, 28 }, "LFO", &g_patch.lfo_enable);
        patch_changed |= Slider((Rectangle){ 660, 105, 130, 12 }, "LFO RATE", &g_patch.lfo_rate, 0, 7);
        patch_changed |= Slider((Rectangle){ 660, 145, 130, 12 }, "PMS (vibrato)", &g_patch.pms, 0, 7);
        patch_changed |= Slider((Rectangle){ 830, 145, 130, 12 }, "AMS (tremolo)", &g_patch.ams, 0, 3);
        patch_changed |= Toggle((Rectangle){ 830, 55, 130, 28 }, "AM ENABLE", &g_am_enable);

        /* Presets */
        for (i = 0; i < genisys_preset_count() && i < 6; i++) {
            Rectangle b = (Rectangle){ 660.0f + i * 78, 178, 72, 22 };
            if (Button(b, genisys_preset_name(i), 0)) {
                g_patch = genisys_preset_patch(i);
                patch_changed = 1;
            }
        }
        DrawText("PRESETS", 660, 165, 11, (Color){ 160, 160, 170, 255 });

        /* Operator panels: 8 sliders (30px spacing) + SSG-EG row + mute/solo/KS row + envelope graph */
        for (op = 0; op < 4; op++) {
            float px = 40.0f + op * 260.0f;
            float py = 250.0f;
            char title[8];
            GenisysOperatorParams *o = &g_patch.op[op];
            snprintf(title, sizeof title, "OP%d", op + 1);
            DrawText(title, (int)px, (int)(py - 25), 18, (Color){ 90, 170, 250, 255 });

            patch_changed |= Slider((Rectangle){ px, py + 10, 200, 12 }, "MUL (harmonic ratio)", &o->mul, 0, 15);
            patch_changed |= Slider((Rectangle){ px, py + 40, 200, 12 }, "DT (detune)", &o->dt, 0, 7);
            patch_changed |= Slider((Rectangle){ px, py + 70, 200, 12 }, "TL (0=loudest)", &o->tl, 0, 127);
            patch_changed |= Slider((Rectangle){ px, py + 100, 200, 12 }, "AR (attack speed)", &o->ar, 0, 31);
            patch_changed |= Slider((Rectangle){ px, py + 130, 200, 12 }, "D1R (decay speed)", &o->d1r, 0, 31);
            patch_changed |= Slider((Rectangle){ px, py + 160, 200, 12 }, "D2R (sustain decay)", &o->d2r, 0, 31);
            patch_changed |= Slider((Rectangle){ px, py + 190, 200, 12 }, "SL (sustain level)", &o->sl, 0, 15);
            patch_changed |= Slider((Rectangle){ px, py + 220, 200, 12 }, "RR (release speed)", &o->rr, 0, 15);

            patch_changed |= Toggle((Rectangle){ px, py + 245, 60, 24 }, "SSG", &o->ssg_enable);
            patch_changed |= Slider((Rectangle){ px + 90, py + 251, 110, 12 }, "SSG MODE", &o->ssg_mode, 0, 7);

            patch_changed |= Toggle((Rectangle){ px, py + 282, 55, 22 }, "MUTE", &g_op_mute[op]);
            patch_changed |= Toggle((Rectangle){ px + 65, py + 282, 55, 22 }, "SOLO", &g_op_solo[op]);
            patch_changed |= Slider((Rectangle){ px + 135, py + 287, 65, 12 }, "KEY SCALE", &o->ks, 0, 3);

            draw_envelope_graph((Rectangle){ px, py + 313, 200, 50 }, o);
        }

        /* Octave controls, next to the piano */
        {
            int oct_down_clicked = Button((Rectangle){ 40, PIANO_Y, 40, 34 }, "OCT-", 0);
            int oct_up_clicked = Button((Rectangle){ 40, PIANO_Y + 44, 40, 34 }, "OCT+", 0);
            if (oct_down_clicked && g_octave > OCTAVE_MIN) g_octave--;
            if (oct_up_clicked && g_octave < OCTAVE_MAX) g_octave++;
            DrawText(TextFormat("Octave %+d", g_octave), 40, PIANO_Y + 86, 14, (Color){ 200, 200, 210, 255 });
        }

        /* PSG panel: to the right of the piano, same row */
        {
            float qx = 700.0f, qy = (float)PIANO_Y;
            DrawText("PSG (SN76489)", (int)qx, (int)(qy - 22), 15, (Color){ 90, 170, 250, 255 });
            psg_changed |= Slider((Rectangle){ qx, qy + 10, 160, 12 }, "PSG LEVEL (layers under FM)", &g_psg_ui.level, 0, 15);

            DrawText("NOISE", (int)qx, (int)(qy + 45), 13, (Color){ 160, 160, 170, 255 });
            psg_changed |= Toggle((Rectangle){ qx, qy + 62, 70, 24 }, g_psg_ui.noise_on ? "ON" : "OFF", &g_psg_ui.noise_on);
            psg_changed |= Toggle((Rectangle){ qx + 80, qy + 62, 90, 24 }, g_psg_ui.noise_white ? "WHITE" : "PERIODIC", &g_psg_ui.noise_white);
            psg_changed |= Slider((Rectangle){ qx, qy + 105, 160, 12 }, "NOISE VOLUME", &g_psg_ui.noise_volume, 0, 15);
            psg_changed |= Slider((Rectangle){ qx, qy + 135, 160, 12 }, "NOISE RATE", &g_psg_ui.noise_rate, 0, 3);
        }

        /* Hand changes to the audio thread. If a queue is momentarily full,
         * keep the change pending and retry next frame rather than lose it. */
        if (patch_changed) patch_dirty = 1;
        if (psg_changed) psg_dirty = 1;
        if (patch_dirty) {
            Event ev = { 0 };
            ev.type = EV_PATCH;
            ev.patch = effective_patch();
            if (queue_push(&g_ui_queue, &ev)) patch_dirty = 0;
        }
        if (psg_dirty) {
            Event ev = { 0 };
            ev.type = EV_PSG;
            ev.psg = g_psg_ui;
            if (queue_push(&g_ui_queue, &ev)) psg_dirty = 0;
        }

        {
            Rectangle rec_rect = { 950, 15, 90, 28 };
            Vector2 mouse = GetMousePosition();
            int hot = CheckCollisionPointRec(mouse, rec_rect);
            Color col = g_recording ? (Color){ 220, 60, 60, 255 } : (hot ? (Color){ 65, 65, 78, 255 } : (Color){ 42, 42, 50, 255 });
            DrawRectangleRec(rec_rect, col);
            DrawRectangleLinesEx(rec_rect, 1, (Color){ 80, 80, 90, 255 });
            DrawText(g_recording ? "REC..." : "* REC", (int)rec_rect.x + 14, (int)rec_rect.y + 7, 13, RAYWHITE);
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (g_recording) {
                    g_recording = 0;
                    if (g_record_frames > 0) {
                        Wave w = { 0 };
                        w.frameCount = (unsigned int)g_record_frames;
                        w.sampleRate = OUTPUT_SAMPLE_RATE;
                        w.sampleSize = 16;
                        w.channels = 2;
                        w.data = g_record_buffer;
                        ExportWave(w, TextFormat("genesis_synth_recording_%d.wav", record_index++));
                    }
                } else {
                    g_recording = 1;
                    g_record_frames = 0;
                }
            }
        }
        draw_piano();
        DrawText("Play: mouse, or Z S X D C V G B H N J M , -- octave: [ ]", PIANO_X, PIANO_Y + WHITE_KEY_H + 15, 14, (Color){ 160, 160, 170, 255 });
        EndDrawing();
    }

    midi_input_shutdown();
    UnloadAudioStream(stream);
    CloseAudioDevice();
    free(g_record_buffer);
    CloseWindow();
    return 0;
}

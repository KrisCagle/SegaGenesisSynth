#include "genisys_presets.h"

#include <stddef.h>

/* ---- How presets are written ------------------------------------------
 *
 * Operators are listed OP1..OP4. Which ones you hear depends on the
 * algorithm: carriers are OP4 (algorithms 0-3), OP2+OP4 (4), OP2-4 (5, 6),
 * all four (7). The rest are modulators: their level (TL) sets brightness.
 *
 * O(mul, dt, tl, ar, d1r, d2r, sl, rr, ks), all in YM2612 register units:
 *   mul  frequency ratio (0 = x0.5)      dt  detune (0 none, 1-3 up, 5-7 down)
 *   tl   level, 0 = loudest (0.75 dB/step)
 *   ar   attack (31 instant)  d1r/d2r  decay rates  sl  sustain (15 = silent)
 *   rr   release (15 fastest)  ks  key scaling (higher notes decay faster)
 * OA(...) is the same with LFO tremolo enabled; OS(..., ssg) adds an SSG-EG
 * shape (0x08 | mode). OFF is a silent operator.
 *
 * `level` is a carrier TL offset found by tools/preset_levels so presets
 * play at similar loudness; tests/engine_tests.c checks it stays true. */

typedef struct {
    int mul, dt, tl, ar, d1r, d2r, sl, rr, ks, am, ssg;
} OpDef;

#define O(mul, dt, tl, ar, d1r, d2r, sl, rr, ks) { mul, dt, tl, ar, d1r, d2r, sl, rr, ks, 0, 0 }
#define OA(mul, dt, tl, ar, d1r, d2r, sl, rr, ks) { mul, dt, tl, ar, d1r, d2r, sl, rr, ks, 1, 0 }
#define OS(mul, dt, tl, ar, d1r, d2r, sl, rr, ks, ssg) { mul, dt, tl, ar, d1r, d2r, sl, rr, ks, 0, ssg }
#define OFF { 1, 0, 127, 31, 0, 0, 0, 15, 0, 0, 0 }

typedef struct {
    const char *category;
    const char *name;
    int alg, fb;
    int lfo, lfo_rate, ams, pms;   /* chip LFO: on/off, rate 0-7, tremolo, vibrato */
    OpDef op[4];
    int vel;                       /* velocity sensitivity %; 0 = engine default */
    int no_vel;                    /* 1 = ignore velocity (organs) */
    int mode, glide, unison, detune, center; /* center: keep unison copies un-panned */
    int psg_mode, psg_level, psg_octave, psg_arp_speed;
    int chorus, echo, echo_fb, echo_div, reverb, reverb_size;
    int level;
} PresetDef;

enum { UNI = GENISYS_PSG_UNISON, ARP = GENISYS_PSG_ARPEGGIO };
enum { MONO = GENISYS_MODE_MONO, LEGATO = GENISYS_MODE_LEGATO };
enum { D4 = 0, D8 = 1, D8DOT = 2, D16 = 3, D4DOT = 4, D8TRI = 5 };

static const PresetDef PRESETS[] = {
    { .category = "Init", .name = "Init", .level = -2,
      .alg = 0, .fb = 0,
      .op = { O(1, 4, 22, 31, 10, 0, 4, 8, 0), O(1, 4, 26, 31, 10, 0, 4, 8, 0),
              O(1, 4, 30, 31, 10, 0, 4, 8, 0), O(1, 4, 5, 31, 10, 0, 4, 8, 0) } },

    /* ===== Bass ===== */
    { .category = "Bass", .name = "Slap Bass", .level = 3, .alg = 4, .fb = 4, .vel = 70,
      .op = { O(1, 0, 14, 31, 16, 0, 11, 10, 1), O(1, 0, 6, 31, 12, 2, 7, 9, 1),
              O(3, 0, 22, 31, 15, 0, 12, 10, 1), O(1, 0, 4, 31, 6, 2, 3, 9, 1) } },
    { .category = "Bass", .name = "Rubber Bass", .level = 1, .alg = 0, .fb = 5, .vel = 60,
      .op = { O(1, 0, 32, 31, 10, 0, 6, 10, 0), O(2, 0, 40, 31, 8, 0, 4, 10, 0),
              O(1, 0, 24, 31, 14, 0, 8, 10, 0), O(1, 0, 2, 31, 4, 1, 2, 10, 0) } },
    { .category = "Bass", .name = "Finger Bass", .level = 2, .alg = 2, .fb = 3, .vel = 60,
      .op = { O(1, 0, 38, 31, 8, 0, 4, 9, 0), O(2, 0, 44, 31, 10, 0, 6, 9, 0),
              O(1, 0, 30, 31, 12, 0, 7, 9, 0), O(1, 0, 3, 30, 5, 2, 3, 9, 1) } },
    { .category = "Bass", .name = "Saw Bass", .level = -3, .alg = 4, .fb = 6, .mode = MONO, .glide = 40,
      .op = { O(1, 0, 14, 31, 4, 0, 2, 10, 0), O(1, 0, 6, 31, 3, 1, 2, 10, 0),
              O(0, 0, 38, 31, 6, 0, 4, 10, 0), O(0, 0, 10, 31, 2, 0, 1, 10, 0) } },
    { .category = "Bass", .name = "Acid Bass", .level = 2, .alg = 0, .fb = 6, .mode = LEGATO, .glide = 80,
      .op = { O(1, 0, 30, 31, 9, 0, 6, 10, 0), O(1, 0, 34, 31, 9, 0, 6, 10, 0),
              O(1, 0, 6, 31, 16, 0, 10, 10, 0), O(1, 0, 2, 31, 2, 0, 1, 11, 0) } },
    { .category = "Bass", .name = "Sub Bass", .level = 2, .alg = 7, .fb = 0,
      .op = { OFF, O(2, 0, 32, 31, 4, 0, 3, 9, 0), OFF, O(1, 0, 2, 31, 1, 0, 1, 9, 0) } },
    { .category = "Bass", .name = "Grit Bass", .level = 5, .alg = 1, .fb = 7, .unison = 2, .detune = 8, .center = 1,
      .op = { O(1, 0, 20, 31, 6, 0, 3, 10, 0), O(1, 0, 30, 31, 8, 0, 5, 10, 0),
              O(2, 0, 34, 31, 10, 0, 6, 10, 0), O(1, 0, 4, 31, 3, 1, 2, 10, 0) } },
    { .category = "Bass", .name = "Night Drive Bass", .level = 12, .alg = 0, .fb = 6, .unison = 2, .detune = 6, .center = 1, .vel = 60,
      .op = { O(1, 0, 24, 31, 12, 0, 8, 10, 0), O(1, 0, 36, 31, 10, 0, 6, 10, 0),
              O(2, 0, 20, 31, 16, 0, 9, 10, 0), O(1, 0, 3, 31, 5, 2, 3, 10, 1) } },

    /* ===== Lead ===== */
    { .category = "Lead", .name = "Square Lead", .level = -6, .alg = 4, .fb = 0, .lfo = 1, .lfo_rate = 3, .pms = 1,
      .op = { O(2, 0, 26, 31, 0, 0, 0, 9, 0), O(1, 0, 8, 31, 2, 0, 1, 9, 0),
              O(4, 0, 40, 31, 0, 0, 0, 9, 0), O(2, 0, 24, 31, 2, 0, 1, 9, 0) } },
    { .category = "Lead", .name = "Saw Lead", .level = 4, .alg = 4, .fb = 6, .mode = LEGATO, .glide = 30, .unison = 2, .detune = 10,
      .op = { O(1, 0, 20, 31, 0, 0, 0, 9, 0), O(1, 0, 6, 31, 1, 0, 1, 9, 0),
              O(1, 0, 24, 31, 0, 0, 0, 9, 0), O(1, 1, 10, 31, 1, 0, 1, 9, 0) } },
    { .category = "Lead", .name = "Fifth Lead", .level = -3, .alg = 4, .fb = 3, .echo = 20, .echo_fb = 30, .echo_div = D8DOT,
      .op = { O(2, 0, 28, 31, 0, 0, 0, 9, 0), O(2, 0, 8, 31, 1, 0, 1, 9, 0),
              O(3, 0, 30, 31, 0, 0, 0, 9, 0), O(3, 0, 12, 31, 1, 0, 1, 9, 0) } },
    { .category = "Lead", .name = "Whistle", .alg = 7, .fb = 0, .mode = LEGATO, .glide = 60,
      .lfo = 1, .lfo_rate = 3, .pms = 2, .reverb = 20, .reverb_size = 50,
      .op = { OFF, O(4, 0, 42, 24, 0, 0, 0, 8, 0), OFF, O(2, 0, 4, 24, 0, 0, 0, 8, 0) } },
    { .category = "Lead", .name = "Hero Lead", .level = -2, .alg = 3, .fb = 5, .unison = 2, .detune = 8,
      .echo = 25, .echo_fb = 35, .echo_div = D8DOT,
      .op = { O(1, 0, 24, 31, 0, 0, 0, 9, 0), O(1, 0, 30, 31, 0, 0, 0, 9, 0),
              O(2, 0, 34, 31, 4, 0, 2, 9, 0), O(1, 0, 4, 30, 2, 0, 1, 9, 0) } },
    { .category = "Lead", .name = "Pulse + PSG", .level = 3, .alg = 4, .fb = 2, .psg_mode = UNI, .psg_level = 12,
      .op = { O(2, 0, 30, 31, 0, 0, 0, 9, 0), O(1, 0, 10, 31, 2, 0, 1, 9, 0),
              O(1, 0, 40, 31, 0, 0, 0, 9, 0), O(1, 0, 14, 31, 2, 0, 1, 9, 0) } },
    { .category = "Lead", .name = "Screamer", .level = -3, .alg = 0, .fb = 7, .mode = MONO, .glide = 50,
      .lfo = 1, .lfo_rate = 4, .pms = 3,
      .op = { O(1, 0, 18, 31, 0, 0, 0, 9, 0), O(1, 0, 26, 31, 0, 0, 0, 9, 0),
              O(2, 0, 30, 31, 0, 0, 0, 9, 0), O(1, 0, 4, 31, 0, 0, 0, 9, 0) } },
    { .category = "Lead", .name = "Soft Sine Lead", .alg = 7, .fb = 0, .lfo = 1, .lfo_rate = 3, .pms = 1,
      .reverb = 25, .reverb_size = 60,
      .op = { OFF, O(2, 0, 36, 28, 0, 0, 0, 8, 0), OFF, O(1, 0, 4, 28, 0, 0, 0, 8, 0) } },

    /* ===== Keys ===== */
    { .category = "Keys", .name = "Tine E.Piano", .level = 1, .alg = 4, .fb = 0, .vel = 70, .chorus = 30,
      .op = { O(14, 0, 40, 31, 20, 0, 15, 9, 2), O(1, 0, 6, 31, 7, 2, 4, 7, 2),
              O(1, 0, 32, 31, 9, 0, 6, 8, 1), O(1, 1, 6, 31, 5, 2, 3, 7, 1) } },
    { .category = "Keys", .name = "Clav", .level = -4, .alg = 4, .fb = 3, .vel = 70,
      .op = { O(5, 0, 22, 31, 14, 0, 8, 10, 2), O(1, 0, 6, 31, 10, 3, 6, 11, 2),
              O(1, 0, 26, 31, 14, 0, 8, 10, 0), O(2, 0, 14, 31, 10, 0, 6, 11, 2) } },
    { .category = "Keys", .name = "Harpsichord", .level = -2, .alg = 2, .fb = 4,
      .op = { O(4, 0, 30, 31, 12, 0, 6, 8, 2), O(2, 0, 36, 31, 10, 0, 5, 8, 0),
              O(3, 0, 28, 31, 10, 0, 6, 8, 0), O(1, 0, 4, 31, 9, 4, 7, 8, 2) } },
    { .category = "Keys", .name = "Toy Piano", .alg = 4, .fb = 0,
      .op = { O(7, 0, 30, 31, 16, 0, 12, 9, 2), O(2, 0, 8, 31, 10, 0, 15, 9, 2),
              O(3, 0, 34, 31, 14, 0, 10, 9, 0), O(1, 0, 8, 31, 9, 0, 15, 9, 2) } },
    { .category = "Keys", .name = "Honky Piano", .level = 1, .alg = 4, .fb = 1, .vel = 70,
      .op = { O(1, 0, 28, 31, 12, 0, 8, 8, 2), O(1, 2, 6, 31, 7, 3, 5, 7, 2),
              O(1, 0, 30, 31, 12, 0, 8, 8, 2), O(1, 6, 6, 31, 7, 3, 5, 7, 2) } },
    { .category = "Keys", .name = "Glass Keys", .level = -3, .alg = 5, .fb = 0, .reverb = 30, .reverb_size = 60,
      .op = { O(7, 0, 36, 31, 10, 0, 8, 7, 0), O(1, 0, 8, 31, 6, 2, 4, 6, 1),
              O(2, 0, 14, 31, 8, 0, 6, 6, 1), O(4, 0, 22, 31, 12, 0, 10, 6, 2) } },

    /* ===== Organ ===== */
    { .category = "Organ", .name = "Drawbar Organ", .level = 3, .alg = 7, .fb = 0, .no_vel = 1, .chorus = 35,
      .op = { O(0, 0, 12, 31, 0, 0, 0, 12, 0), O(1, 0, 8, 31, 0, 0, 0, 12, 0),
              O(2, 0, 14, 31, 0, 0, 0, 12, 0), O(3, 0, 20, 31, 0, 0, 0, 12, 0) } },
    { .category = "Organ", .name = "Jazz Organ", .level = 3, .alg = 7, .fb = 0, .no_vel = 1, .chorus = 25,
      .op = { O(1, 0, 8, 31, 0, 0, 0, 12, 0), O(2, 0, 16, 31, 0, 0, 0, 12, 0),
              O(0, 0, 12, 31, 0, 0, 0, 12, 0), O(3, 0, 14, 31, 12, 0, 15, 12, 0) } },
    { .category = "Organ", .name = "Cathedral Organ", .level = 1, .alg = 7, .fb = 0, .no_vel = 1,
      .reverb = 40, .reverb_size = 85,
      .op = { O(1, 0, 10, 22, 0, 0, 0, 8, 0), O(2, 0, 12, 22, 0, 0, 0, 8, 0),
              O(4, 0, 16, 22, 0, 0, 0, 8, 0), O(8, 0, 24, 22, 0, 0, 0, 8, 0) } },

    /* ===== Bells & Mallets ===== */
    { .category = "Bells & Mallets", .name = "Marimba", .level = -2, .alg = 4, .fb = 0, .vel = 70,
      .op = { O(4, 0, 34, 31, 22, 0, 15, 10, 2), O(1, 0, 6, 31, 12, 0, 15, 10, 2),
              O(10, 0, 46, 31, 24, 0, 15, 10, 2), O(1, 0, 14, 31, 14, 0, 15, 10, 2) } },
    { .category = "Bells & Mallets", .name = "Vibraphone", .level = -4, .alg = 4, .fb = 0, .lfo = 1, .lfo_rate = 3, .ams = 2,
      .reverb = 25, .reverb_size = 55,
      .op = { O(4, 0, 40, 31, 14, 0, 15, 8, 1), OA(1, 0, 6, 31, 5, 0, 15, 6, 1),
              O(1, 0, 44, 31, 10, 0, 15, 6, 0), OA(4, 0, 26, 31, 8, 0, 15, 6, 1) } },
    { .category = "Bells & Mallets", .name = "Glockenspiel", .level = -4, .alg = 5, .fb = 0, .reverb = 20, .reverb_size = 50,
      .op = { O(11, 0, 38, 31, 14, 0, 12, 8, 2), O(2, 0, 8, 31, 8, 0, 15, 8, 2),
              O(4, 0, 16, 31, 10, 0, 15, 8, 2), O(6, 0, 26, 31, 12, 0, 15, 8, 2) } },
    { .category = "Bells & Mallets", .name = "Tubular Bell", .level = -3, .alg = 5, .fb = 0, .reverb = 30, .reverb_size = 70,
      .op = { O(7, 3, 30, 31, 6, 0, 10, 5, 0), O(1, 0, 8, 31, 4, 0, 15, 4, 1),
              O(2, 2, 14, 31, 5, 0, 15, 4, 1), O(5, 6, 20, 31, 7, 0, 15, 4, 1) } },
    { .category = "Bells & Mallets", .name = "Music Box", .level = -5, .alg = 4, .fb = 0,
      .echo = 20, .echo_fb = 25, .echo_div = D4, .reverb = 25, .reverb_size = 50,
      .op = { O(6, 0, 34, 31, 16, 0, 15, 8, 2), O(2, 0, 8, 31, 9, 0, 15, 8, 2),
              O(3, 0, 38, 31, 14, 0, 15, 8, 0), O(4, 0, 20, 31, 10, 0, 15, 8, 2) } },
    { .category = "Bells & Mallets", .name = "Steel Drum", .level = -1, .alg = 4, .fb = 0,
      .op = { O(1, 0, 26, 31, 14, 0, 10, 8, 0), O(1, 0, 6, 31, 8, 0, 15, 8, 1),
              O(3, 3, 36, 31, 12, 0, 12, 8, 0), O(2, 0, 16, 31, 10, 0, 15, 8, 0) } },

    /* ===== Brass & Winds ===== */
    { .category = "Brass & Winds", .name = "Brass Section", .level = 3, .alg = 4, .fb = 5, .unison = 2, .detune = 6,
      .reverb = 15, .reverb_size = 50,
      .op = { O(1, 0, 24, 21, 6, 0, 3, 9, 0), O(1, 0, 6, 25, 3, 1, 2, 9, 0),
              O(1, 0, 28, 20, 6, 0, 3, 9, 0), O(1, 1, 8, 25, 3, 1, 2, 9, 0) } },
    { .category = "Brass & Winds", .name = "Synth Brass", .level = -4, .alg = 4, .fb = 6, .unison = 2, .detune = 12, .chorus = 30,
      .op = { O(1, 0, 22, 19, 8, 0, 4, 9, 0), O(1, 0, 6, 27, 2, 0, 1, 9, 0),
              O(2, 0, 34, 18, 8, 0, 5, 9, 0), O(1, 7, 10, 27, 2, 0, 1, 9, 0) } },
    { .category = "Brass & Winds", .name = "Trumpet", .alg = 0, .fb = 4, .mode = LEGATO, .glide = 20,
      .lfo = 1, .lfo_rate = 3, .pms = 1,
      .op = { O(1, 0, 30, 22, 6, 0, 3, 9, 0), O(1, 0, 28, 22, 6, 0, 3, 9, 0),
              O(1, 0, 22, 23, 5, 0, 3, 9, 0), O(1, 0, 4, 26, 2, 0, 1, 9, 0) } },
    { .category = "Brass & Winds", .name = "Flute", .level = 1, .alg = 4, .fb = 7, .mode = LEGATO, .glide = 25,
      .lfo = 1, .lfo_rate = 3, .pms = 1, .reverb = 20, .reverb_size = 50,
      .op = { O(1, 0, 52, 24, 0, 0, 0, 8, 0), O(1, 0, 30, 24, 0, 0, 0, 8, 0),
              O(1, 0, 50, 24, 0, 0, 0, 8, 0), O(1, 0, 4, 24, 0, 0, 0, 8, 0) } },
    { .category = "Brass & Winds", .name = "Clarinet", .level = 1, .alg = 4, .fb = 0, .mode = LEGATO, .glide = 20,
      .op = { O(2, 0, 30, 24, 0, 0, 0, 9, 0), O(1, 0, 6, 25, 0, 0, 0, 9, 0),
              O(2, 0, 40, 24, 0, 0, 0, 9, 0), O(1, 0, 20, 25, 0, 0, 0, 9, 0) } },
    { .category = "Brass & Winds", .name = "Oboe", .level = -4, .alg = 0, .fb = 2, .mode = LEGATO, .glide = 20,
      .lfo = 1, .lfo_rate = 3, .pms = 1,
      .op = { O(1, 0, 34, 24, 0, 0, 0, 9, 0), O(3, 0, 32, 24, 0, 0, 0, 9, 0),
              O(1, 0, 26, 24, 0, 0, 0, 9, 0), O(1, 0, 6, 25, 0, 0, 0, 9, 0) } },

    /* ===== Strings & Pads ===== */
    { .category = "Strings & Pads", .name = "Strings", .alg = 2, .fb = 3, .unison = 2, .detune = 14,
      .chorus = 35, .reverb = 25, .reverb_size = 65,
      .op = { O(1, 0, 34, 16, 0, 0, 0, 6, 0), O(2, 0, 40, 16, 0, 0, 0, 6, 0),
              O(1, 0, 30, 16, 0, 0, 0, 6, 0), O(1, 0, 6, 17, 0, 0, 0, 6, 0) } },
    { .category = "Strings & Pads", .name = "Slow Pad", .level = -1, .alg = 5, .fb = 2, .unison = 2, .detune = 10,
      .reverb = 40, .reverb_size = 80,
      .op = { O(1, 0, 34, 14, 0, 0, 0, 5, 0), O(1, 0, 10, 14, 0, 0, 0, 4, 0),
              O(2, 0, 16, 13, 0, 0, 0, 4, 0), O(1, 2, 12, 14, 0, 0, 0, 4, 0) } },
    { .category = "Strings & Pads", .name = "Glass Pad", .level = -4, .alg = 5, .fb = 0, .lfo = 1, .lfo_rate = 2, .ams = 1,
      .reverb = 40, .reverb_size = 75,
      .op = { O(3, 0, 32, 14, 0, 0, 0, 5, 0), OA(1, 0, 10, 15, 0, 0, 0, 4, 0),
              OA(2, 0, 16, 15, 0, 0, 0, 4, 0), OA(4, 0, 24, 15, 0, 0, 0, 4, 0) } },
    { .category = "Strings & Pads", .name = "Choir Ahh", .level = -6, .alg = 4, .fb = 1, .unison = 2, .detune = 8,
      .lfo = 1, .lfo_rate = 3, .pms = 1, .reverb = 35, .reverb_size = 70,
      .op = { O(1, 0, 36, 15, 0, 0, 0, 6, 0), O(1, 0, 8, 15, 0, 0, 0, 5, 0),
              O(3, 0, 40, 15, 0, 0, 0, 6, 0), O(3, 0, 20, 15, 0, 0, 0, 5, 0) } },
    { .category = "Strings & Pads", .name = "SSG Sweep", .level = 3, .alg = 4, .fb = 3, .reverb = 30, .reverb_size = 65,
      .op = { OS(1, 0, 22, 20, 8, 0, 8, 6, 0, 0x0A), O(1, 0, 8, 18, 0, 0, 0, 5, 0),
              OS(2, 0, 30, 18, 6, 0, 10, 6, 0, 0x0E), O(1, 1, 10, 18, 0, 0, 0, 5, 0) } },
    { .category = "Strings & Pads", .name = "Dark Atmosphere", .level = -3, .alg = 5, .fb = 4,
      .echo = 30, .echo_fb = 45, .echo_div = D4DOT, .reverb = 45, .reverb_size = 90,
      .op = { O(0, 0, 30, 12, 0, 0, 0, 4, 0), O(0, 0, 10, 12, 0, 0, 0, 3, 0),
              O(1, 3, 16, 11, 0, 0, 0, 3, 0), O(1, 7, 18, 12, 0, 0, 0, 3, 0) } },

    /* ===== Guitar ===== */
    { .category = "Guitar", .name = "Distortion Guitar", .level = -1, .alg = 0, .fb = 7, .echo = 12, .echo_fb = 20, .echo_div = D8,
      .op = { O(1, 0, 18, 31, 2, 0, 1, 8, 0), O(1, 0, 24, 31, 3, 0, 2, 8, 0),
              O(2, 0, 30, 31, 3, 0, 2, 8, 0), O(1, 0, 2, 31, 3, 2, 2, 8, 0) } },
    { .category = "Guitar", .name = "Power Chord", .level = -2, .alg = 1, .fb = 7, .unison = 2, .detune = 10,
      .op = { O(1, 0, 20, 31, 2, 0, 1, 8, 0), O(2, 0, 22, 31, 2, 0, 1, 8, 0),
              O(1, 0, 24, 31, 3, 0, 2, 8, 0), O(1, 0, 2, 31, 3, 2, 2, 8, 0) } },
    { .category = "Guitar", .name = "Clean Guitar", .level = -5, .alg = 4, .fb = 2, .vel = 70, .chorus = 30,
      .op = { O(3, 0, 32, 31, 16, 0, 10, 9, 2), O(1, 0, 6, 31, 8, 0, 15, 8, 2),
              O(1, 0, 30, 31, 12, 0, 8, 9, 0), O(2, 0, 16, 31, 9, 0, 15, 8, 2) } },
    { .category = "Guitar", .name = "Muted Guitar", .level = -3, .alg = 4, .fb = 3,
      .op = { O(1, 0, 28, 31, 20, 0, 15, 12, 0), O(1, 0, 6, 31, 16, 0, 15, 12, 1),
              O(2, 0, 34, 31, 22, 0, 15, 12, 0), O(1, 0, 10, 31, 16, 0, 15, 12, 0) } },

    /* ===== Plucks ===== */
    { .category = "Plucks", .name = "Harp", .alg = 4, .fb = 0, .reverb = 30, .reverb_size = 65,
      .op = { O(2, 0, 38, 31, 12, 0, 12, 7, 1), O(1, 0, 6, 31, 7, 0, 15, 6, 2),
              O(1, 0, 44, 31, 10, 0, 12, 7, 0), O(2, 0, 18, 31, 8, 0, 15, 6, 2) } },
    { .category = "Plucks", .name = "Koto", .level = -1, .alg = 4, .fb = 2, .reverb = 20, .reverb_size = 50,
      .op = { O(3, 0, 26, 31, 16, 0, 12, 8, 2), O(1, 0, 6, 31, 9, 0, 15, 8, 2),
              O(7, 0, 40, 31, 20, 0, 15, 8, 0), O(2, 0, 14, 31, 10, 0, 15, 8, 2) } },
    { .category = "Plucks", .name = "Pizzicato", .level = -2, .alg = 2, .fb = 2, .reverb = 25, .reverb_size = 60,
      .op = { O(1, 0, 34, 31, 14, 0, 10, 10, 0), O(2, 0, 38, 31, 14, 0, 10, 10, 0),
              O(1, 0, 28, 31, 14, 0, 10, 10, 0), O(1, 0, 4, 31, 12, 0, 15, 10, 2) } },
    { .category = "Plucks", .name = "Banjo", .level = -5, .alg = 4, .fb = 4,
      .op = { O(5, 0, 24, 31, 16, 0, 10, 9, 2), O(1, 0, 6, 31, 10, 0, 15, 9, 2),
              O(2, 0, 30, 31, 16, 0, 12, 9, 0), O(3, 0, 18, 31, 12, 0, 15, 9, 2) } },
    { .category = "Plucks", .name = "Kalimba", .level = -4, .alg = 4, .fb = 0, .echo = 15, .echo_fb = 25, .echo_div = D8DOT,
      .op = { O(3, 0, 36, 31, 18, 0, 15, 9, 2), O(1, 0, 6, 31, 10, 0, 15, 9, 2),
              OFF, O(6, 0, 30, 31, 16, 0, 15, 9, 2) } },

    /* ===== FM Percussion ===== */
    { .category = "FM Percussion", .name = "Timpani", .level = -1, .alg = 4, .fb = 3, .reverb = 30, .reverb_size = 70,
      .op = { O(1, 0, 26, 31, 10, 0, 8, 6, 0), O(1, 0, 4, 31, 6, 0, 15, 6, 0),
              O(2, 3, 34, 31, 12, 0, 12, 6, 0), O(1, 7, 10, 31, 7, 0, 15, 6, 0) } },
    { .category = "FM Percussion", .name = "Cowbell", .level = -3, .alg = 4, .fb = 0,
      .op = { O(5, 0, 38, 31, 14, 0, 15, 10, 0), O(3, 0, 8, 31, 12, 0, 15, 10, 1),
              O(7, 0, 40, 31, 14, 0, 15, 10, 0), O(5, 2, 12, 31, 12, 0, 15, 10, 1) } },
    { .category = "FM Percussion", .name = "Woodblock", .level = -14, .alg = 4, .fb = 0,
      .op = { O(2, 0, 36, 31, 24, 0, 15, 13, 0), O(3, 0, 6, 31, 18, 0, 15, 13, 0),
              O(5, 0, 40, 31, 26, 0, 15, 13, 0), O(1, 0, 14, 31, 18, 0, 15, 13, 0) } },
    { .category = "FM Percussion", .name = "Orchestra Hit", .level = -1, .alg = 5, .fb = 4, .unison = 2, .detune = 10,
      .reverb = 40, .reverb_size = 75,
      .op = { O(1, 0, 24, 31, 10, 0, 8, 9, 0), O(1, 0, 8, 31, 9, 0, 15, 8, 0),
              O(2, 0, 12, 31, 9, 0, 15, 8, 0), O(3, 0, 16, 31, 9, 0, 15, 8, 0) } },
    { .category = "FM Percussion", .name = "Metal Hit", .level = -3, .alg = 4, .fb = 6, .reverb = 25, .reverb_size = 60,
      .op = { O(11, 3, 24, 31, 12, 0, 15, 7, 0), O(1, 0, 6, 31, 8, 0, 15, 7, 0),
              O(7, 5, 28, 31, 10, 0, 15, 7, 0), O(3, 0, 12, 31, 8, 0, 15, 7, 0) } },

    /* ===== Sound Effects ===== */
    { .category = "SFX", .name = "Coin Chime", .level = -1, .alg = 4, .fb = 0, .echo = 30, .echo_fb = 30, .echo_div = D16,
      .op = { O(8, 0, 40, 31, 14, 0, 15, 8, 0), O(4, 0, 6, 31, 10, 0, 15, 8, 2),
              O(6, 0, 40, 31, 14, 0, 15, 8, 0), O(8, 0, 12, 31, 10, 0, 15, 8, 0) } },
    { .category = "SFX", .name = "Ray Gun", .level = -2, .alg = 0, .fb = 7, .mode = MONO, .lfo = 1, .lfo_rate = 7, .pms = 7,
      .op = { O(1, 0, 24, 31, 8, 0, 8, 10, 0), O(3, 0, 28, 31, 8, 0, 8, 10, 0),
              O(1, 0, 20, 31, 10, 0, 10, 10, 0), O(1, 0, 4, 31, 10, 0, 15, 10, 0) } },
    { .category = "SFX", .name = "Explosion", .level = -5, .alg = 4, .fb = 7, .reverb = 30, .reverb_size = 70,
      .op = { O(1, 0, 0, 31, 6, 0, 15, 6, 0), O(0, 0, 4, 31, 6, 0, 15, 6, 0),
              O(1, 0, 0, 31, 8, 0, 15, 6, 0), O(0, 0, 8, 31, 7, 0, 15, 6, 0) } },
    { .category = "SFX", .name = "Siren", .alg = 7, .fb = 0, .mode = MONO, .lfo = 1, .lfo_rate = 0, .pms = 7,
      .op = { OFF, O(2, 0, 26, 31, 0, 0, 0, 10, 0), OFF, O(1, 0, 4, 31, 0, 0, 0, 10, 0) } },

    /* ===== Chip (PSG layers) ===== */
    { .category = "Chip", .name = "PSG Square", .level = -5, .alg = 7, .fb = 0, .psg_mode = UNI, .psg_level = 15,
      .op = { OFF, OFF, OFF, OFF } },
    { .category = "Chip", .name = "Chip Arpeggio", .level = -9, .alg = 4, .fb = 0, .psg_mode = ARP, .psg_level = 13,
      .psg_octave = 1, .psg_arp_speed = 2,
      .op = { O(2, 0, 30, 31, 0, 0, 0, 9, 0), O(1, 0, 14, 31, 2, 0, 1, 9, 0), OFF, OFF } },
    { .category = "Chip", .name = "FM + Square", .level = -5, .alg = 4, .fb = 0, .psg_mode = UNI, .psg_level = 11,
      .op = { O(2, 0, 26, 31, 0, 0, 0, 9, 0), O(1, 0, 8, 31, 2, 0, 1, 9, 0),
              O(4, 0, 40, 31, 0, 0, 0, 9, 0), O(2, 0, 24, 31, 2, 0, 1, 9, 0) } },
    { .category = "Chip", .name = "Chip Bell Arp", .level = -3, .alg = 5, .fb = 0, .psg_mode = ARP, .psg_level = 10,
      .psg_octave = 1, .psg_arp_speed = 3, .echo = 20, .echo_fb = 30, .echo_div = D8,
      .op = { O(11, 0, 38, 31, 14, 0, 12, 8, 2), O(2, 0, 8, 31, 8, 0, 15, 8, 2),
              O(4, 0, 16, 31, 10, 0, 15, 8, 2), O(6, 0, 26, 31, 12, 0, 15, 8, 2) } },
};

#define PRESET_COUNT ((int)(sizeof PRESETS / sizeof PRESETS[0]))

int genisys_preset_count(void) {
    return PRESET_COUNT;
}

const char *genisys_preset_name(int index) {
    return (index >= 0 && index < PRESET_COUNT) ? PRESETS[index].name : NULL;
}

const char *genisys_preset_category(int index) {
    return (index >= 0 && index < PRESET_COUNT) ? PRESETS[index].category : NULL;
}

GenisysPatch genisys_preset_patch(int index) {
    GenisysPatch p = genisys_default_patch();
    const PresetDef *d;
    uint8_t carriers;
    int i;

    if (index < 0 || index >= PRESET_COUNT) return p;
    d = &PRESETS[index];
    carriers = genisys_carrier_mask(d->alg);

    p.algorithm = d->alg;
    p.feedback = d->fb;
    p.lfo_enable = d->lfo;
    p.lfo_rate = d->lfo ? d->lfo_rate : 3;
    p.ams = d->ams;
    p.pms = d->pms;
    if (d->no_vel) p.velocity_sens = 0;
    else if (d->vel > 0) p.velocity_sens = d->vel;
    p.voice_mode = d->mode;
    p.glide_time = d->glide;
    p.unison = d->unison > 0 ? d->unison : 1;
    if (d->detune > 0) p.unison_detune = d->detune;
    p.unison_stereo = !d->center;

    for (i = 0; i < 4; i++) {
        const OpDef *s = &d->op[i];
        GenisysOperatorParams *o = &p.op[i];
        int tl = s->tl;
        /* Loudness normalisation applies to carriers only, so it changes
         * volume without changing tone. Silent operators stay silent. */
        if ((carriers & (1 << i)) && tl < 127) tl += d->level;
        o->mul = s->mul;
        o->dt = s->dt;
        o->tl = tl < 0 ? 0 : (tl > 127 ? 127 : tl);
        o->ar = s->ar;
        o->d1r = s->d1r;
        o->d2r = s->d2r;
        o->sl = s->sl;
        o->rr = s->rr;
        o->ks = s->ks;
        o->am = s->am;
        o->ssg_enable = (s->ssg & 0x08) ? 1 : 0;
        o->ssg_mode = s->ssg & 0x07;
    }
    return p;
}

GenisysPsgSettings genisys_preset_psg(int index) {
    GenisysPsgSettings s = genisys_default_psg();
    const PresetDef *d;

    if (index < 0 || index >= PRESET_COUNT) return s;
    d = &PRESETS[index];
    s.mode = d->psg_mode;
    if (d->psg_level > 0) s.level = d->psg_level;
    s.octave = d->psg_octave;
    if (d->psg_arp_speed > 0) s.arp_speed = d->psg_arp_speed;
    return s;
}

GenisysPresetFx genisys_preset_fx(int index) {
    GenisysPresetFx fx = { 0, 0, 35, 2, 0, 50 };
    const PresetDef *d;

    if (index < 0 || index >= PRESET_COUNT) return fx;
    d = &PRESETS[index];
    fx.chorus_mix = d->chorus;
    fx.echo_mix = d->echo;
    if (d->echo > 0) {
        fx.echo_feedback = d->echo_fb;
        fx.echo_division = d->echo_div;
    }
    fx.reverb_mix = d->reverb;
    if (d->reverb > 0) fx.reverb_size = d->reverb_size;
    return fx;
}

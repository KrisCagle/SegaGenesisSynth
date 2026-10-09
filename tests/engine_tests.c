/* Tests for the Genisys engine: pitch, sample-rate independence, mixing
 * headroom, voice allocation and velocity. Several of these cover bugs the
 * pre-engine desktop app had (see docs/ROADMAP.md, section 2). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "genisys_engine.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* The engine holds ~100 KB of resampler tables/history: keep it off the stack. */
static GenisysEngine g_engine;

#define MAX_FRAMES (192000 * 3)
static float g_left[MAX_FRAMES];
static float g_right[MAX_FRAMES];

/* One audible operator (OP4, a carrier in every algorithm) playing a plain
 * sine, so frequency and level are easy to measure. */
static GenisysPatch sine_patch(void) {
    GenisysPatch p = genisys_default_patch();
    int i;
    p.algorithm = 7;
    p.feedback = 0;
    p.velocity_sens = 0;
    for (i = 0; i < 4; i++) {
        p.op[i].mul = 1;
        p.op[i].dt = 0;
        p.op[i].tl = (i == 3) ? 0 : 127;
        p.op[i].ar = 31;
        p.op[i].d1r = 0;
        p.op[i].sl = 0;
        p.op[i].rr = 15;
    }
    return p;
}

static void start(double rate, const GenisysPatch *p) {
    genisys_engine_init(&g_engine, rate);
    genisys_engine_set_patch(&g_engine, p);
}

/* Frequency from rising zero crossings, interpolated to sub-sample
 * accuracy, ignoring the first 0.1 s (attack + resampler warm-up). */
static double measure_hz(const float *x, int frames, double rate) {
    int i, first_i = -1, crossings = 0;
    double first_t = 0.0, last_t = 0.0;
    for (i = (int)(rate * 0.1); i < frames - 1; i++) {
        if (x[i] <= 0.0f && x[i + 1] > 0.0f) {
            double t = i + (double)(-x[i]) / (double)(x[i + 1] - x[i]);
            if (first_i < 0) { first_i = i; first_t = t; }
            last_t = t;
            crossings++;
        }
    }
    if (crossings < 2) return 0.0;
    return (crossings - 1) / ((last_t - first_t) / rate);
}

static double rms(const float *x, int from, int to) {
    double sum = 0.0;
    int i;
    for (i = from; i < to; i++) sum += (double)x[i] * x[i];
    return sqrt(sum / (to - from));
}

static double play_and_measure_hz(double rate, int note) {
    GenisysPatch p = sine_patch();
    int frames = (int)(rate * 1.1);
    start(rate, &p);
    genisys_engine_note_on(&g_engine, note, 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    return measure_hz(g_left, frames, rate);
}

static double note_hz(int note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }

/* ---- Bug #1: notes above ~832 Hz all played the same wrong pitch ---- */
static void test_pitch_across_keyboard(void) {
    static const int NOTES[] = { 33, 45, 60, 69, 81, 84, 96, 108 };
    int i;
    for (i = 0; i < (int)(sizeof NOTES / sizeof NOTES[0]); i++) {
        double want = note_hz(NOTES[i]);
        double got = play_and_measure_hz(48000.0, NOTES[i]);
        char msg[128];
        snprintf(msg, sizeof msg, "MIDI note %d plays %.2f Hz (want %.2f, within 0.3%%)", NOTES[i], got, want);
        CHECK(fabs(got / want - 1.0) < 0.003, msg);
    }
}

static void test_block_fnum_mapping(void) {
    int block, fnum;
    genisys_note_to_block_fnum(60, &block, &fnum);
    CHECK(block == 4 && fnum == 644, "C4 maps to block 4, fnum 644 (the usual sound-driver value)");
    genisys_note_to_block_fnum(72, &block, &fnum);
    CHECK(block == 5 && fnum == 644, "C5 maps to block 5, fnum 644");
    genisys_note_to_block_fnum(127, &block, &fnum);
    CHECK(block == 7 && fnum == 2047, "notes past the chip's ~6.6 kHz ceiling clamp to block 7, fnum 2047");
}

/* ---- Bug #5: pitch and level must not depend on the host sample rate ---- */
static double render_a4(double rate, double *level) {
    GenisysPatch p = sine_patch();
    int frames = (int)(rate * 1.1);
    start(rate, &p);
    genisys_engine_note_on(&g_engine, 69, 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    *level = rms(g_left, (int)(rate * 0.1), frames);
    return measure_hz(g_left, frames, rate);
}

static void test_sample_rate_independence(void) {
    static const double RATES[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    double ref_level, level, hz;
    int i;

    render_a4(48000.0, &ref_level);
    for (i = 0; i < (int)(sizeof RATES / sizeof RATES[0]); i++) {
        char msg[128];
        hz = render_a4(RATES[i], &level);
        snprintf(msg, sizeof msg, "A4 at a %.0f Hz host rate plays %.2f Hz (want 440, within 0.3%%)", RATES[i], hz);
        CHECK(fabs(hz / 440.0 - 1.0) < 0.003, msg);
        snprintf(msg, sizeof msg, "level at %.0f Hz is %+.2f dB vs 48 kHz (within 0.5 dB)",
                 RATES[i], 20.0 * log10(level / ref_level));
        CHECK(fabs(20.0 * log10(level / ref_level)) < 0.5, msg);
    }
}

/* ---- Bug #4: a loud chord must exceed full scale, not hard-clip at it ---- */
static void test_loud_chord_not_clipped(void) {
    GenisysPatch p = sine_patch();
    static const int NOTES[6] = { 36, 48, 60, 72, 84, 96 };
    int i, frames = 4800;
    float peak = 0.0f;

    for (i = 0; i < 4; i++) p.op[i].tl = 0; /* every operator a full-volume carrier */
    start(48000.0, &p);
    for (i = 0; i < 6; i++) genisys_engine_note_on(&g_engine, NOTES[i], 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    for (i = 0; i < frames; i++) {
        float a = fabsf(g_left[i]);
        if (a > peak) peak = a;
    }
    CHECK(peak > 1.05f, "6 full-volume voices peak above 1.0 instead of hard-clipping at 1.0");
}

/* ---- Bug #3: the 7th note used to be silently dropped ---- */
static void test_seventh_note_steals_oldest(void) {
    GenisysPatch p = sine_patch();
    int i, v, stolen = -1, others_intact = 1;

    start(48000.0, &p);
    for (i = 0; i < 6; i++) genisys_engine_note_on(&g_engine, 60 + i, 100);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (g_engine.voice[v].note == 60) stolen = v;
    genisys_engine_note_on(&g_engine, 70, 100);

    CHECK(stolen >= 0 && g_engine.voice[stolen].note == 70 && g_engine.voice[stolen].state == GENISYS_VOICE_HELD,
          "7th note takes over the oldest held voice");
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (v != stolen && (g_engine.voice[v].state != GENISYS_VOICE_HELD || g_engine.voice[v].note == 70)) others_intact = 0;
    }
    CHECK(others_intact, "the other 5 held notes are untouched");
}

/* ---- Bug #2: the next note used to grab the voice that was still releasing ---- */
static void test_release_tail_not_cut(void) {
    GenisysPatch p = sine_patch();
    int v, first = -1, second = -1;

    p.op[3].rr = 2; /* long release, still audible after a short pause */
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_render(&g_engine, g_left, g_right, 2400);
    genisys_engine_note_off(&g_engine, 60);
    genisys_engine_render(&g_engine, g_left, g_right, 2400);
    genisys_engine_note_on(&g_engine, 62, 100);

    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (g_engine.voice[v].note == 60 && g_engine.voice[v].state == GENISYS_VOICE_RELEASED) first = v;
        if (g_engine.voice[v].note == 62 && g_engine.voice[v].state == GENISYS_VOICE_HELD) second = v;
    }
    CHECK(first >= 0 && second >= 0 && first != second,
          "a new note uses an idle voice, leaving the previous note's release tail playing");
}

static void test_same_note_retriggers_same_voice(void) {
    GenisysPatch p = sine_patch();
    int v, used = 0;

    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_note_off(&g_engine, 60);
    genisys_engine_note_on(&g_engine, 60, 100);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (g_engine.voice[v].state != GENISYS_VOICE_FREE) used++;
    CHECK(used == 1, "repeating a note retriggers its voice instead of stacking a second one");
}

/* ---- Bug #8: velocity used to be ignored ---- */
static double level_at_velocity(int velocity, int sens) {
    GenisysPatch p = sine_patch();
    p.velocity_sens = sens;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 69, velocity);
    genisys_engine_render(&g_engine, g_left, g_right, 24000);
    return rms(g_left, 4800, 24000);
}

static void test_velocity(void) {
    double loud = level_at_velocity(127, 100);
    double soft = level_at_velocity(64, 100);
    double db = 20.0 * log10(soft / loud);
    char msg[128];

    snprintf(msg, sizeof msg, "velocity 64 at 100%% sensitivity is %.1f dB quieter (want ~11.9 +/- 1)", -db);
    CHECK(fabs(db + 11.9) < 1.0, msg);

    loud = level_at_velocity(127, 0);
    soft = level_at_velocity(10, 0);
    CHECK(fabs(20.0 * log10(soft / loud)) < 0.1, "at 0% sensitivity every velocity plays equally loud");
}

static void test_velocity_only_touches_carriers(void) {
    GenisysPatch p = genisys_default_patch(); /* algorithm 0: only OP4 is a carrier */
    int v, mod_ok, car_ok;

    p.velocity_sens = 100;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 1);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (g_engine.voice[v].state == GENISYS_VOICE_HELD) break;

    /* The chip stores TL in its 0-1023 envelope domain: register value << 3. */
    mod_ok = g_engine.chip.channel[v].op[0].tl == (uint32_t)(p.op[0].tl << 3);
    car_ok = g_engine.chip.channel[v].op[3].tl > (uint32_t)(p.op[3].tl << 3);
    CHECK(mod_ok, "velocity leaves modulator levels alone (they set the timbre)");
    CHECK(car_ok, "velocity lowers carrier levels (they set the volume)");
}

/* ---- Console output stage: DAC model, DC blocker, filter ---- */

static void start_console(double rate, const GenisysPatch *p, int model, int filter_on) {
    GenisysConsoleSettings c = genisys_default_console();
    c.chip_model = model;
    c.filter_on = filter_on;
    start(rate, p);
    genisys_engine_set_console(&g_engine, &c);
}

static void test_default_console_is_model1(void) {
    GenisysConsoleSettings c = genisys_default_console();
    CHECK(c.chip_model == GENISYS_CHIP_YM2612 && c.filter_on && c.filter_hz == GENISYS_MODEL1_FILTER_HZ,
          "default console: YM2612 (ladder effect) with the Model 1-style filter");
}

static void test_ladder_offset_never_reaches_output(void) {
    GenisysPatch p = sine_patch();
    int i, frames = 48000;
    float worst = 0.0f;

    start_console(48000.0, &p, GENISYS_CHIP_YM2612, 1);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    for (i = 0; i < frames; i++) if (fabsf(g_left[i]) > worst) worst = fabsf(g_left[i]);
    CHECK(worst == 0.0f, "YM2612 mode: silence is exactly 0 (the ladder's DC offset is blocked)");

    genisys_engine_note_on(&g_engine, 69, 127);
    genisys_engine_render(&g_engine, g_left, g_right, 4800);
    genisys_engine_note_off(&g_engine, 69);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    worst = 0.0f;
    for (i = frames - 4800; i < frames; i++) if (fabsf(g_left[i]) > worst) worst = fabsf(g_left[i]);
    CHECK(worst < 1e-3f, "YM2612 mode: output settles back to 0 after a note ends");
}

/* Relative size of the difference between the YM2612 and YM3438 renders
 * of the same note: how much the ladder effect changes it. */
static double ladder_distortion(int tl) {
    GenisysPatch p = sine_patch();
    int frames = 24000, i;
    double diff = 0.0, ref = 0.0;

    p.op[3].tl = tl;
    start_console(48000.0, &p, GENISYS_CHIP_YM3438, 0);
    genisys_engine_note_on(&g_engine, 57, 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);   /* clean-ish reference in g_left */

    start_console(48000.0, &p, GENISYS_CHIP_YM2612, 0);
    genisys_engine_note_on(&g_engine, 57, 127);
    genisys_engine_render(&g_engine, g_right, g_right + MAX_FRAMES / 2, frames); /* ladder version in g_right */

    for (i = 4800; i < frames; i++) {
        double d = (double)g_right[i] - g_left[i];
        diff += d * d;
        ref += (double)g_left[i] * g_left[i];
    }
    return sqrt(diff / ref);
}

static void test_ladder_effect_hits_quiet_notes(void) {
    double loud = ladder_distortion(0);
    double quiet = ladder_distortion(40);
    char msg[160];

    snprintf(msg, sizeof msg, "ladder effect barely changes a loud note (%.1f%% difference, want < 5%%)", loud * 100.0);
    CHECK(loud < 0.05, msg);
    snprintf(msg, sizeof msg, "ladder effect strongly distorts a quiet note (%.0f%% difference, want > 30%%)", quiet * 100.0);
    CHECK(quiet > 0.30, msg);
}

static double filter_gain_db(int note) {
    GenisysPatch p = sine_patch();
    int frames = 24000;
    double off, on;

    start_console(48000.0, &p, GENISYS_CHIP_CLEAN, 0);
    genisys_engine_note_on(&g_engine, note, 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    off = rms(g_left, 4800, frames);

    start_console(48000.0, &p, GENISYS_CHIP_CLEAN, 1);
    genisys_engine_note_on(&g_engine, note, 127);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    on = rms(g_left, 4800, frames);
    return 20.0 * log10(on / off);
}

static void test_console_filter_response(void) {
    /* A first-order low-pass at fc attenuates f by 10*log10(1 + (f/fc)^2) dB. */
    static const int NOTES[] = { 57, 81, 99, 111 };
    int i;
    for (i = 0; i < 4; i++) {
        double f = note_hz(NOTES[i]);
        double want = -10.0 * log10(1.0 + pow(f / GENISYS_MODEL1_FILTER_HZ, 2.0));
        double got = filter_gain_db(NOTES[i]);
        char msg[160];
        snprintf(msg, sizeof msg, "console filter at %.0f Hz: %.2f dB (first-order 3.68 kHz curve says %.2f, within 0.5)",
                 f, got, want);
        CHECK(fabs(got - want) < 0.5, msg);
    }
}

int main(void) {
    genisys_engine_global_init();

    test_block_fnum_mapping();
    test_pitch_across_keyboard();
    test_sample_rate_independence();
    test_loud_chord_not_clipped();
    test_seventh_note_steals_oldest();
    test_release_tail_not_cut();
    test_same_note_retriggers_same_voice();
    test_velocity();
    test_velocity_only_touches_carriers();
    test_default_console_is_model1();
    test_ladder_offset_never_reaches_output();
    test_ladder_effect_hits_quiet_notes();
    test_console_filter_response();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

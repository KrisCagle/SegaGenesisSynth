/* Tests for the Genisys engine: pitch, sample-rate independence, mixing
 * headroom, voice allocation and velocity. Several of these cover bugs the
 * pre-engine desktop app had (see docs/ROADMAP.md, section 2). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "genisys_drums.h"
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

/* ---- Drum kit ---- */

static void enable_drums(int enabled) {
    GenisysDrumSettings d;
    d.enabled = enabled;
    d.level = 100;
    genisys_engine_set_drums(&g_engine, &d);
}

static void test_drum_kit_mapping(void) {
    CHECK(genisys_drum_for_note(36)->kind == GENISYS_DRUM_DAC && genisys_drum_for_note(38)->kind == GENISYS_DRUM_DAC,
          "kick (36) and snare (38) are DAC samples");
    CHECK(genisys_drum_for_note(42)->kind == GENISYS_DRUM_NOISE && genisys_drum_for_note(49)->kind == GENISYS_DRUM_NOISE,
          "closed hat (42) and crash (49) use the PSG noise channel");
    CHECK(genisys_drum_for_note(60)->kind == GENISYS_DRUM_NONE, "unmapped notes are not drums");
}

static void test_drums_take_fm_channel_6(void) {
    GenisysPatch p = sine_patch();
    int i, v, used_six = 0;

    start(48000.0, &p);
    enable_drums(1);
    for (i = 0; i < 6; i++) genisys_engine_note_on(&g_engine, 60 + i, 100);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (v == 5 && g_engine.voice[v].state != GENISYS_VOICE_FREE) used_six = 1;
    CHECK(!used_six, "with drums on, FM never uses channel 6 (the DAC's)");
    CHECK(g_engine.chip.dac_enable, "with drums on, the chip's DAC mode is enabled");

    enable_drums(0);
    CHECK(!g_engine.chip.dac_enable, "turning drums off gives channel 6 back to FM");
}

static double drum_rms(int note, int velocity, int drums_on) {
    GenisysPatch p = sine_patch();
    start_console(48000.0, &p, GENISYS_CHIP_CLEAN, 0);
    enable_drums(drums_on);
    genisys_engine_drum_hit(&g_engine, note, velocity);
    genisys_engine_render(&g_engine, g_left, g_right, 9600);
    return rms(g_left, 0, 9600);
}

static void test_drum_hits(void) {
    double kick = drum_rms(36, 127, 1);
    double soft_kick = drum_rms(36, 40, 1);
    double hat = drum_rms(42, 127, 1);
    char msg[128];

    CHECK(kick > 0.01, "a kick hit sounds through the DAC");
    snprintf(msg, sizeof msg, "a soft kick is quieter (%.1f dB)", 20.0 * log10(soft_kick / kick));
    CHECK(soft_kick < kick * 0.5, msg);
    CHECK(hat > 0.001, "a closed hat sounds through the PSG noise channel");
    CHECK(drum_rms(36, 127, 0) == 0.0, "drum hits are ignored while drums are off");
    CHECK(drum_rms(60, 127, 1) == 0.0, "unmapped drum notes are silent");
}

static void test_dac_drum_ends_in_silence(void) {
    GenisysPatch p = sine_patch();
    int i;
    float tail = 0.0f;

    start_console(48000.0, &p, GENISYS_CHIP_CLEAN, 0);
    enable_drums(1);
    genisys_engine_drum_hit(&g_engine, 38, 127);
    genisys_engine_render(&g_engine, g_left, g_right, 48000);
    for (i = 38400; i < 48000; i++) if (fabsf(g_left[i]) > tail) tail = fabsf(g_left[i]);
    CHECK(tail < 1e-3f && g_engine.dac_sample == NULL, "a drum sample plays once and returns the DAC to silence");
}

/* ---- PSG layer ---- */

/* The tone register value the PSG uses for a frequency. */
static int psg_divider(double hz) { return (int)(PSG_CLOCK_HZ / (32.0 * hz) + 0.5); }

static void set_psg(int mode, int attack) {
    GenisysPsgSettings s = genisys_default_psg();
    s.mode = mode;
    s.level = 15;
    s.attack = attack;
    s.sustain = 15; /* hold at full volume, so tests see the attack/release alone */
    s.arp_speed = 2;
    genisys_engine_set_psg(&g_engine, &s);
}

static void render_frames(int frames) {
    /* 800 output samples at 48 kHz = one 60 Hz frame. */
    genisys_engine_render(&g_engine, g_left, g_right, 800 * frames);
}

static void test_psg_unison_doubles_notes(void) {
    GenisysPatch p = sine_patch();
    start(48000.0, &p);
    set_psg(GENISYS_PSG_UNISON, 0);
    genisys_engine_note_on(&g_engine, 69, 100);
    CHECK(g_engine.psg.tone_reg[0] == psg_divider(440.0) && g_engine.psg.volume[0] == 0,
          "unison: a note starts a PSG channel at the same pitch, full volume (attack 0)");
    genisys_engine_note_off(&g_engine, 69);
    render_frames(40); /* release 2 frames per step x 15 steps, plus margin */
    CHECK(g_engine.psg.volume[0] == 15, "unison: after key-up the PSG channel releases to silence");
}

static void test_psg_low_notes_move_up_octaves(void) {
    GenisysPatch p = sine_patch();
    start(48000.0, &p);
    set_psg(GENISYS_PSG_UNISON, 0);
    genisys_engine_note_on(&g_engine, 33, 100); /* A1, 55 Hz: below the PSG's ~109 Hz floor */
    CHECK(g_engine.psg.tone_reg[0] == psg_divider(110.0),
          "notes below the PSG's range are moved up an octave instead of going out of tune");
}

static void test_psg_attack_steps_per_frame(void) {
    GenisysPatch p = sine_patch();
    start(48000.0, &p);
    set_psg(GENISYS_PSG_UNISON, 2); /* 2 frames per volume step: 30 frames to full */
    genisys_engine_note_on(&g_engine, 69, 100);
    CHECK(g_engine.psg.volume[0] == 15, "attack starts from silence");
    render_frames(10);
    CHECK(g_engine.psg.volume[0] > 0 && g_engine.psg.volume[0] < 15, "after 10 frames the attack is part-way up");
    render_frames(30);
    CHECK(g_engine.psg.volume[0] == 0, "after 40 frames the attack has reached full volume");
}

static void test_psg_arpeggio_cycles_held_notes(void) {
    GenisysPatch p = sine_patch();
    int seen[3] = { 0, 0, 0 }, f, k;
    static const int NOTES[3] = { 60, 64, 67 };

    start(48000.0, &p);
    set_psg(GENISYS_PSG_ARPEGGIO, 0);
    for (k = 0; k < 3; k++) genisys_engine_note_on(&g_engine, NOTES[k], 100);
    for (f = 0; f < 12; f++) {
        render_frames(1);
        for (k = 0; k < 3; k++) {
            if (g_engine.psg.tone_reg[0] == psg_divider(note_hz(NOTES[k]))) seen[k] = 1;
        }
    }
    CHECK(seen[0] && seen[1] && seen[2], "arpeggio: one PSG channel cycles through every held note");
    CHECK(g_engine.psg.volume[1] == 15 && g_engine.psg.volume[2] == 15, "arpeggio: the other PSG channels stay silent");
}

/* ---- Volume math in the PSG's logarithmic steps (2 dB each) ---- */

static void test_noise_drum_velocity_in_db(void) {
    GenisysPatch p = sine_patch();
    GenisysDrumSettings d = { 1, 100 };

    start(48000.0, &p);
    genisys_engine_set_drums(&g_engine, &d);
    genisys_engine_drum_hit(&g_engine, 42, 127);
    CHECK(g_engine.psg.volume[3] == 15 - genisys_drum_for_note(42)->start_volume,
          "a full-velocity hat plays at the drum's own volume");
    genisys_engine_drum_hit(&g_engine, 42, 64);
    CHECK(g_engine.psg.volume[3] == 15 - genisys_drum_for_note(42)->start_volume + 6,
          "velocity 64 (-12 dB) lowers a PSG hat by 6 steps of 2 dB, not to a fraction of its volume");
}

static void test_psg_level_adds_in_db(void) {
    GenisysPatch p = sine_patch();
    GenisysPsgSettings s = genisys_default_psg();

    start(48000.0, &p);
    s.mode = GENISYS_PSG_UNISON;
    s.attack = 0;
    s.sustain = 15;
    s.level = 12;
    genisys_engine_set_psg(&g_engine, &s);
    genisys_engine_note_on(&g_engine, 69, 100);
    CHECK(g_engine.psg.volume[0] == 3, "PSG level 12 of 15 is 3 steps (6 dB) quieter than full");
}

/* ---- Expression: pitch bend, mod-wheel vibrato, sustain pedal ---- */

static double bent_hz(int note, double bend) {
    GenisysPatch p = sine_patch();
    int frames = 52800;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, note, 127);
    genisys_engine_pitch_bend(&g_engine, bend);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    return measure_hz(g_left, frames, 48000.0);
}

static void test_pitch_bend(void) {
    static const double BENDS[] = { 2.0, -2.0, 12.0, -12.0, 24.0 };
    int i;
    for (i = 0; i < 5; i++) {
        double want = 440.0 * pow(2.0, BENDS[i] / 12.0), got = bent_hz(69, BENDS[i]);
        char msg[128];
        snprintf(msg, sizeof msg, "A4 bent %+.0f semitones plays %.2f Hz (want %.2f, within 0.3%%)", BENDS[i], got, want);
        CHECK(fabs(got / want - 1.0) < 0.003, msg);
    }
}

/* Ratio of the longest to the shortest cycle: how much the pitch wobbles. */
static double pitch_wobble(double mod) {
    GenisysPatch p = sine_patch();
    int i, frames = 48000, last = -1;
    double shortest = 1e9, longest = 0.0;

    p.vibrato_depth = 50; /* +/- half a semitone at full mod wheel */
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 69, 127);
    genisys_engine_mod_wheel(&g_engine, mod);
    genisys_engine_render(&g_engine, g_left, g_right, frames);
    for (i = 4800; i < frames - 1; i++) {
        if (g_left[i] <= 0.0f && g_left[i + 1] > 0.0f) {
            if (last >= 0) {
                double period = i - last;
                if (period < shortest) shortest = period;
                if (period > longest) longest = period;
            }
            last = i;
        }
    }
    return longest / shortest;
}

static void test_mod_wheel_vibrato(void) {
    double still = pitch_wobble(0.0), full = pitch_wobble(1.0);
    char msg[128];
    snprintf(msg, sizeof msg, "mod wheel at 0: steady pitch (cycle lengths vary %.1f%%)", (still - 1.0) * 100.0);
    CHECK(still < 1.02, msg);
    snprintf(msg, sizeof msg, "mod wheel at full: vibrato (cycle lengths vary %.1f%%, want > 4%%)", (full - 1.0) * 100.0);
    CHECK(full > 1.04, msg);
}

static void test_sustain_pedal(void) {
    GenisysPatch p = sine_patch();
    int v, held = -1;

    start(48000.0, &p);
    genisys_engine_sustain(&g_engine, 1);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_note_off(&g_engine, 60);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (g_engine.voice[v].note == 60) held = v;
    CHECK(held >= 0 && g_engine.voice[held].state == GENISYS_VOICE_HELD,
          "with the pedal down, a released key keeps sounding");
    genisys_engine_sustain(&g_engine, 0);
    CHECK(g_engine.voice[held].state == GENISYS_VOICE_RELEASED, "lifting the pedal releases the note");
}

static void test_psg_follows_bend(void) {
    GenisysPatch p = sine_patch();
    GenisysPsgSettings s = genisys_default_psg();
    start(48000.0, &p);
    s.mode = GENISYS_PSG_UNISON;
    genisys_engine_set_psg(&g_engine, &s);
    genisys_engine_note_on(&g_engine, 69, 100);
    genisys_engine_pitch_bend(&g_engine, 12.0);
    genisys_engine_render(&g_engine, g_left, g_right, 480);
    CHECK(g_engine.psg.tone_reg[0] == psg_divider(880.0), "the PSG layer follows pitch bend");
}

/* ---- Voice modes: unison, mono, legato, glide ---- */

static int held_voices(void) {
    int v, n = 0;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) if (g_engine.voice[v].state == GENISYS_VOICE_HELD) n++;
    return n;
}

static double voice_hz(int v) {
    return g_engine.voice[v].last_fnum * YM2612_SAMPLE_HZ * ldexp(1.0, g_engine.voice[v].last_block - 1) / 1048576.0;
}

static void test_unison(void) {
    GenisysPatch p = sine_patch();
    int v, a = -1, b = -1;

    p.unison = 2;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 69, 100);
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (g_engine.voice[v].state != GENISYS_VOICE_HELD) continue;
        if (a < 0) a = v; else b = v;
    }
    CHECK(held_voices() == 2, "unison 2: one key plays two chip channels");
    CHECK(a >= 0 && b >= 0 && voice_hz(a) != voice_hz(b), "unison 2: the two channels are detuned apart");
    CHECK(a >= 0 && b >= 0 && g_engine.chip.channel[a].pan_l != g_engine.chip.channel[b].pan_l,
          "unison 2 stereo: the channels are panned to opposite sides");

    p.unison = 3;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_note_on(&g_engine, 64, 100);
    genisys_engine_note_on(&g_engine, 67, 100);
    CHECK(held_voices() == 6, "unison 3: two notes fill all six channels; a third steals");
}

static void test_mono_last_note_priority(void) {
    GenisysPatch p = sine_patch();
    p.voice_mode = GENISYS_MODE_MONO;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_note_on(&g_engine, 64, 100);
    CHECK(held_voices() == 1 && g_engine.voice[0].note == 64, "mono: one channel, playing the newest key");
    genisys_engine_note_off(&g_engine, 64);
    CHECK(held_voices() == 1 && g_engine.voice[0].note == 60, "mono: releasing it falls back to the key still held");
    genisys_engine_note_off(&g_engine, 60);
    CHECK(held_voices() == 0, "mono: releasing the last key releases the note");
}

/* Whether a second, overlapping key restarts the carrier's attack. */
static int second_note_restarts_attack(int mode) {
    GenisysPatch p = sine_patch();
    p.voice_mode = mode;
    p.op[3].ar = 10;  /* slow attack, so a restart is visible */
    p.op[3].d1r = 20; /* decay to a quieter sustain first: an attack */
    p.op[3].sl = 10;  /* from full volume would finish instantly */
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 60, 100);
    genisys_engine_render(&g_engine, g_left, g_right, 48000); /* attack finishes */
    genisys_engine_note_on(&g_engine, 64, 100);
    return g_engine.chip.channel[0].op[3].state == YM_EG_ATT;
}

static void test_legato_does_not_retrigger(void) {
    CHECK(second_note_restarts_attack(GENISYS_MODE_MONO), "mono: an overlapping key restarts the envelope");
    CHECK(!second_note_restarts_attack(GENISYS_MODE_LEGATO), "legato: an overlapping key only changes pitch");
}

static void test_glide(void) {
    GenisysPatch p = sine_patch();
    double mid, end;
    char msg[160];

    p.voice_mode = GENISYS_MODE_LEGATO;
    p.glide_time = 100;
    start(48000.0, &p);
    genisys_engine_note_on(&g_engine, 57, 100);  /* A3, 220 Hz */
    genisys_engine_render(&g_engine, g_left, g_right, 4800);
    genisys_engine_note_on(&g_engine, 69, 100);  /* A4, 440 Hz */
    genisys_engine_render(&g_engine, g_left, g_right, 2400); /* 50 ms in */
    mid = voice_hz(0);
    genisys_engine_render(&g_engine, g_left, g_right, 9600); /* 250 ms in */
    end = voice_hz(0);
    snprintf(msg, sizeof msg, "glide 100 ms: half-way through it is between the notes (%.0f Hz)", mid);
    CHECK(mid > 250.0 && mid < 400.0, msg);
    snprintf(msg, sizeof msg, "glide 100 ms: after it ends it sits on the new note (%.1f Hz)", end);
    CHECK(fabs(end / 440.0 - 1.0) < 0.003, msg);
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
    test_drum_kit_mapping();
    test_drums_take_fm_channel_6();
    test_drum_hits();
    test_dac_drum_ends_in_silence();
    test_psg_unison_doubles_notes();
    test_psg_low_notes_move_up_octaves();
    test_psg_attack_steps_per_frame();
    test_psg_arpeggio_cycles_held_notes();
    test_noise_drum_velocity_in_db();
    test_psg_level_adds_in_db();
    test_pitch_bend();
    test_mod_wheel_vibrato();
    test_sustain_pedal();
    test_psg_follows_bend();
    test_unison();
    test_mono_last_note_priority();
    test_legato_does_not_retrigger();
    test_glide();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

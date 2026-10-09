#include "genisys_drums.h"

#include <math.h>
#include <stddef.h>

#define TWO_PI 6.28318530717958647692
#define SR GENISYS_DRUM_SAMPLE_HZ
/* Longest drum: 0.5 s at 13 kHz. A plain integer so the arrays below have a
 * compile-time size in standard C. */
#define MAX_DRUM_LEN 6500

static int8_t g_pool[16 * MAX_DRUM_LEN];
static int g_pool_used = 0;
static float g_tmp[MAX_DRUM_LEN];
static GenisysDrum g_kit[128];

/* xorshift32: tiny, fast and identical on every platform. */
typedef struct { uint32_t s; } Rng;

static float noise(Rng *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return (float)(r->s & 0xFFFFFF) / 8388608.0f - 1.0f;
}

/* Normalizes g_tmp[0..len) to just under full scale and quantizes it to
 * signed 8 bits, the DAC's native format, into the shared pool. */
static const int8_t *finish(int len) {
    int8_t *out = &g_pool[g_pool_used];
    float peak = 0.0f, scale;
    int i;

    for (i = 0; i < len; i++) if (fabsf(g_tmp[i]) > peak) peak = fabsf(g_tmp[i]);
    scale = peak > 0.0f ? 0.95f * 127.0f / peak : 0.0f;
    for (i = 0; i < len; i++) {
        float v = g_tmp[i] * scale;
        out[i] = (int8_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }
    g_pool_used += len;
    return out;
}

static int seconds(double s) { return (int)(SR * s); }

/* Sine with a falling pitch: the body of a kick or tom. */
static void pitched_body(int len, double base_hz, double sweep, double sweep_s, double decay_s, float gain) {
    double phase = 0.0;
    int i;
    for (i = 0; i < len; i++) {
        double t = i / SR;
        double f = base_hz * (1.0 + sweep * exp(-t / sweep_s));
        phase += TWO_PI * f / SR;
        g_tmp[i] += gain * (float)(sin(phase) * exp(-t / decay_s));
    }
}

/* High-passed noise burst: the rattle of a snare, the hiss of a clap. */
static void noise_burst(int len, uint32_t seed, double start_s, double decay_s, float gain, float hp) {
    Rng r = { seed };
    float prev_in = 0.0f, prev_out = 0.0f;
    int i, start = seconds(start_s);
    for (i = start; i < len; i++) {
        double t = (i - start) / SR;
        float x = noise(&r);
        float y = hp * (prev_out + x - prev_in);
        prev_in = x;
        prev_out = y;
        g_tmp[i] += gain * y * (float)exp(-t / decay_s);
    }
}

static void clear(int len) {
    int i;
    for (i = 0; i < len; i++) g_tmp[i] = 0.0f;
}

static void add_dac(int note, const char *name, int len) {
    g_kit[note].kind = GENISYS_DRUM_DAC;
    g_kit[note].name = name;
    g_kit[note].sample = finish(len);
    g_kit[note].length = len;
}

static void add_noise(int note, const char *name, int white, int rate, int volume, int decay_quarters) {
    g_kit[note].kind = GENISYS_DRUM_NOISE;
    g_kit[note].name = name;
    g_kit[note].noise_white = white;
    g_kit[note].noise_rate = rate;
    g_kit[note].start_volume = volume;
    g_kit[note].decay_per_frame = decay_quarters;
}

static void make_tom(int note, const char *name, double hz) {
    int len = seconds(0.45);
    clear(len);
    pitched_body(len, hz, 0.6, 0.04, 0.16, 1.0f);
    noise_burst(seconds(0.03), 0x70A0u + (uint32_t)note, 0.0, 0.008, 0.25f, 0.5f);
    add_dac(note, name, len);
}

void genisys_drums_init(void) {
    int len, i;

    if (g_pool_used > 0) return; /* already built */
    for (i = 0; i < 128; i++) g_kit[i].kind = GENISYS_DRUM_NONE;

    /* 35 Kick 2: tight, punchy */
    len = seconds(0.25);
    clear(len);
    pitched_body(len, 45.0, 3.5, 0.02, 0.08, 1.0f);
    noise_burst(seconds(0.004), 0x35u, 0.0, 0.001, 0.6f, 0.3f);
    add_dac(35, "Kick 2", len);

    /* 36 Kick: round, booming */
    len = seconds(0.35);
    clear(len);
    pitched_body(len, 50.0, 2.6, 0.03, 0.12, 1.0f);
    noise_burst(seconds(0.003), 0x36u, 0.0, 0.001, 0.5f, 0.3f);
    add_dac(36, "Kick", len);

    /* 37 Side Stick */
    len = seconds(0.06);
    clear(len);
    pitched_body(len, 1700.0, 0.0, 1.0, 0.008, 0.8f);
    pitched_body(len, 420.0, 0.0, 1.0, 0.015, 0.6f);
    add_dac(37, "Side Stick", len);

    /* 38 Snare: tone + rattle */
    len = seconds(0.25);
    clear(len);
    pitched_body(len, 185.0, 0.3, 0.01, 0.05, 0.6f);
    pitched_body(len, 330.0, 0.0, 1.0, 0.03, 0.3f);
    noise_burst(len, 0x38u, 0.0, 0.09, 0.8f, 0.6f);
    add_dac(38, "Snare", len);

    /* 39 Clap: three quick bursts and a tail */
    len = seconds(0.25);
    clear(len);
    noise_burst(seconds(0.010), 0x391u, 0.000, 0.004, 1.0f, 0.7f);
    noise_burst(seconds(0.020), 0x392u, 0.010, 0.004, 1.0f, 0.7f);
    noise_burst(seconds(0.030), 0x393u, 0.020, 0.004, 1.0f, 0.7f);
    noise_burst(len, 0x394u, 0.030, 0.08, 0.8f, 0.7f);
    add_dac(39, "Clap", len);

    /* 40 Snare 2: brighter and shorter */
    len = seconds(0.18);
    clear(len);
    pitched_body(len, 220.0, 0.4, 0.008, 0.035, 0.5f);
    noise_burst(len, 0x40u, 0.0, 0.06, 1.0f, 0.8f);
    add_dac(40, "Snare 2", len);

    make_tom(41, "Low Floor Tom", 80.0);
    make_tom(43, "High Floor Tom", 100.0);
    make_tom(45, "Low Tom", 120.0);
    make_tom(47, "Low-Mid Tom", 145.0);
    make_tom(48, "Hi-Mid Tom", 170.0);
    make_tom(50, "High Tom", 200.0);

    /* Hats and cymbals on the PSG noise channel. Decay is in quarter
     * volume steps per 60 Hz frame. */
    add_noise(42, "Closed Hat", 1, 0, 13, 12);
    add_noise(44, "Pedal Hat", 1, 0, 10, 10);
    add_noise(46, "Open Hat", 1, 0, 13, 2);
    add_noise(49, "Crash", 1, 1, 15, 1);
    add_noise(51, "Ride", 1, 0, 10, 1);
    add_noise(57, "Crash 2", 1, 1, 14, 1);
}

const GenisysDrum *genisys_drum_for_note(int note) {
    static const GenisysDrum none = { GENISYS_DRUM_NONE, NULL, NULL, 0, 0, 0, 0, 0 };
    if (note < 0 || note > 127) return &none;
    return &g_kit[note];
}

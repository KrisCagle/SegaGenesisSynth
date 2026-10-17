#ifndef GENISYS_PRESET_FINGERPRINT_H
#define GENISYS_PRESET_FINGERPRINT_H

/* A "fingerprint" of how a factory preset sounds, for spotting presets that
 * sound too much alike. Shared by tools/preset_similarity.c and
 * tests/engine_tests.c.
 *
 * One note is played (as for loudness, in the preset's natural range):
 *   - tone: the spectrum's shape (log energy in 20 bands, 60 Hz - 12 kHz)
 *     at the attack, early in the note and later in the note, with
 *     loudness removed, so only the colour of the sound counts;
 *   - envelope: how the level moves over time (24 points from 5 ms to
 *     1.6 s, including the release after the key goes up).
 * Effects aren't included: they belong to the plugin. */

#include <math.h>

#include "genisys_engine.h"
#include "genisys_presets.h"
#include "preset_loudness.h" /* preset_loudness_note() */

#define FP_RATE 48000
#define FP_HOLD_FRAMES (FP_RATE * 10 / 10)    /* 1.0 s held */
#define FP_TOTAL_FRAMES (FP_RATE * 16 / 10)   /* then 0.6 s of release */
#define FP_FFT_SIZE 2048
#define FP_BANDS 20
#define FP_SNAPSHOTS 3
#define FP_ENV_POINTS 24
#define FP_PI 3.14159265358979323846

typedef struct {
    double tone[FP_SNAPSHOTS][FP_BANDS]; /* dB, mean removed per snapshot */
    double env[FP_ENV_POINTS];        /* dB relative to the peak, floor -60 */
} PresetFingerprint;

static float fp_left[FP_TOTAL_FRAMES], fp_right[FP_TOTAL_FRAMES], fp_mono[FP_TOTAL_FRAMES];

/* In-place iterative radix-2 FFT. */
static inline void fp_fft(double *re, double *im, int n) {
    int i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * FP_PI / len;
        for (i = 0; i < n; i += len) {
            int k;
            for (k = 0; k < len / 2; k++) {
                double wr = cos(ang * k), wi = sin(ang * k);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
        }
    }
}

static inline void fp_tone_snapshot(int start, double out[FP_BANDS]) {
    static double re[FP_FFT_SIZE], im[FP_FFT_SIZE];
    double mean = 0.0;
    int i, b;
    for (i = 0; i < FP_FFT_SIZE; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * FP_PI * i / (FP_FFT_SIZE - 1));
        re[i] = (start + i < FP_TOTAL_FRAMES ? fp_mono[start + i] : 0.0) * w;
        im[i] = 0.0;
    }
    fp_fft(re, im, FP_FFT_SIZE);
    for (b = 0; b < FP_BANDS; b++) {
        double lo = 60.0 * pow(200.0, (double)b / FP_BANDS), hi = 60.0 * pow(200.0, (double)(b + 1) / FP_BANDS);
        int k0 = (int)(lo * FP_FFT_SIZE / FP_RATE), k1 = (int)(hi * FP_FFT_SIZE / FP_RATE);
        double energy = 1e-12;
        int k;
        if (k1 <= k0) k1 = k0 + 1;
        for (k = k0; k < k1 && k < FP_FFT_SIZE / 2; k++) energy += re[k] * re[k] + im[k] * im[k];
        out[b] = 10.0 * log10(energy / (k1 - k0));
    }
    for (b = 0; b < FP_BANDS; b++) mean += out[b];
    mean /= FP_BANDS;
    for (b = 0; b < FP_BANDS; b++) {
        out[b] -= mean;
        if (out[b] < -60.0) out[b] = -60.0;
    }
}

static inline void preset_fingerprint(GenisysEngine *e, int index, PresetFingerprint *fp) {
    static const double snapshot_seconds[FP_SNAPSHOTS] = { 0.01, 0.15, 0.7 };
    GenisysPatch patch = genisys_preset_patch(index);
    GenisysPsgSettings psg = genisys_preset_psg(index);
    const int note = preset_loudness_note(index);
    double peak = 1e-12;
    int i, p;

    genisys_engine_init(e, FP_RATE);
    genisys_engine_set_patch(e, &patch);
    genisys_engine_set_psg(e, &psg);
    genisys_engine_note_on(e, note, 100);
    genisys_engine_render(e, fp_left, fp_right, FP_HOLD_FRAMES);
    genisys_engine_note_off(e, note);
    genisys_engine_render(e, fp_left + FP_HOLD_FRAMES, fp_right + FP_HOLD_FRAMES, FP_TOTAL_FRAMES - FP_HOLD_FRAMES);
    for (i = 0; i < FP_TOTAL_FRAMES; i++) fp_mono[i] = 0.5f * (fp_left[i] + fp_right[i]);

    for (p = 0; p < FP_SNAPSHOTS; p++) fp_tone_snapshot((int)(snapshot_seconds[p] * FP_RATE), fp->tone[p]);

    /* Envelope: 10 ms RMS windows at log-spaced times. */
    for (p = 0; p < FP_ENV_POINTS; p++) {
        double t = 0.005 * pow(1.6 / 0.005, (double)p / (FP_ENV_POINTS - 1));
        int centre = (int)(t * FP_RATE), half = FP_RATE / 200, start = centre - half, count = 0;
        double sum = 0.0;
        if (start < 0) start = 0;
        for (i = start; i < centre + half && i < FP_TOTAL_FRAMES; i++, count++) sum += (double)fp_mono[i] * fp_mono[i];
        fp->env[p] = count > 0 ? sum / count : 0.0;
        if (fp->env[p] > peak) peak = fp->env[p];
    }
    for (p = 0; p < FP_ENV_POINTS; p++) {
        double db = 10.0 * log10(fp->env[p] / peak + 1e-12);
        fp->env[p] = db < -60.0 ? -60.0 : db;
    }
}

/* Distance in dB-ish units: RMS difference of the tone shapes plus half
 * the RMS difference of the envelopes. */
static inline double preset_fingerprint_distance(const PresetFingerprint *a, const PresetFingerprint *b) {
    double tone = 0.0, env = 0.0;
    int p, k;
    for (p = 0; p < FP_SNAPSHOTS; p++)
        for (k = 0; k < FP_BANDS; k++) tone += (a->tone[p][k] - b->tone[p][k]) * (a->tone[p][k] - b->tone[p][k]);
    for (k = 0; k < FP_ENV_POINTS; k++) env += (a->env[k] - b->env[k]) * (a->env[k] - b->env[k]);
    return sqrt(tone / (FP_SNAPSHOTS * FP_BANDS)) + 0.5 * sqrt(env / FP_ENV_POINTS);
}

#endif /* GENISYS_PRESET_FINGERPRINT_H */

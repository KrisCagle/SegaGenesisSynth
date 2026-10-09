/* Renders short demo WAVs through the Genisys engine, so the sound can be
 * heard without a DAW:
 *
 *   genisys_render <output-directory>
 *
 *   genisys_demo.wav          an original 8-bar pattern: FM bass, FM chords
 *                             with a PSG arpeggio, and the drum kit -- three
 *                             engines mixed, like three plugin instances
 *   genisys_chip_compare.wav  one bass riff three times: YM2612 (Model 1),
 *                             YM3438 (Model 2), then Clean with no filter
 *   genisys_preset_tour.wav   every factory preset playing a short phrase
 *                             suited to it, with genisys_preset_tour.txt
 *                             listing when each one starts. Effects are a
 *                             plugin feature, so the tour plays them dry. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genisys_engine.h"
#include "genisys_presets.h"

#define RATE 48000
#define BPM 120.0
#define STEP_FRAMES ((int)(RATE * 60.0 / BPM / 4.0)) /* one 16th note */

/* ---- WAV output ---- */

static void put_u32(FILE *f, unsigned v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put_u16(FILE *f, unsigned v) {
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static int write_wav(const char *path, const float *left, const float *right, int frames) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36u + (unsigned)frames * 4u); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 2);
    put_u32(f, RATE); put_u32(f, RATE * 4); put_u16(f, 4); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, (unsigned)frames * 4u);
    for (i = 0; i < frames; i++) {
        float l = left[i] * 32767.0f, r = right[i] * 32767.0f;
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        put_u16(f, (unsigned)(short)l);
        put_u16(f, (unsigned)(short)r);
    }
    fclose(f);
    return 1;
}

/* ---- Helpers ---- */

static int preset_index(const char *name) {
    int i;
    for (i = 0; i < genisys_preset_count(); i++) {
        if (strcmp(genisys_preset_name(i), name) == 0) return i;
    }
    return 0;
}

static void mix_in(GenisysEngine *e, float *left, float *right, int frames, float gain) {
    static float l[4096], r[4096];
    int done = 0, i;
    while (done < frames) {
        int n = frames - done > 4096 ? 4096 : frames - done;
        genisys_engine_render(e, l, r, n);
        for (i = 0; i < n; i++) {
            left[done + i] += l[i] * gain;
            right[done + i] += r[i] * gain;
        }
        done += n;
    }
}

/* ---- The demo pattern (original): A minor, Am - F - C - G ---- */

static const int ROOTS[4] = { 45, 41, 48, 43 };            /* A2 F2 C3 G2 */
static const int CHORDS[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 55, 60, 64 }, { 55, 59, 62 } };
static const int BASS_STEPS[8] = { 0, 0, 12, 0, 0, 12, 0, 7 }; /* per 8th note, above the root */
static const int TOM_FILL[4] = { 50, 48, 47, 45 };            /* high tom down to low tom */

static void render_demo(const char *dir) {
    static GenisysEngine bass, keys, drums;
    const int bars = 8, steps = bars * 16;
    const int frames = (steps + 32) * STEP_FRAMES; /* + 2 bars of tail */
    float *left = calloc((size_t)frames, sizeof(float));
    float *right = calloc((size_t)frames, sizeof(float));
    char path[1024];
    int step, bass_note = -1;

    GenisysPatch bass_patch = genisys_preset_patch(preset_index("Bass"));
    GenisysPatch keys_patch = genisys_preset_patch(preset_index("E.Piano"));
    GenisysPsgSettings arp = genisys_default_psg();
    GenisysDrumSettings kit = { 1, 90 };

    genisys_engine_init(&bass, RATE);
    genisys_engine_init(&keys, RATE);
    genisys_engine_init(&drums, RATE);
    genisys_engine_set_patch(&bass, &bass_patch);
    genisys_engine_set_patch(&keys, &keys_patch);
    arp.mode = GENISYS_PSG_ARPEGGIO;
    arp.octave = 1;
    arp.level = 9;
    arp.arp_speed = 3;
    genisys_engine_set_psg(&keys, &arp);
    genisys_engine_set_drums(&drums, &kit);

    for (step = 0; step < steps + 32; step++) {
        int offset = step * STEP_FRAMES;
        int bar = step / 16, s = step % 16, chord = bar % 4, k;

        if (step < steps) {
            /* Bass: 8th notes */
            if (s % 2 == 0) {
                if (bass_note >= 0) genisys_engine_note_off(&bass, bass_note);
                bass_note = ROOTS[chord] + BASS_STEPS[s / 2];
                genisys_engine_note_on(&bass, bass_note, s == 0 ? 120 : 96);
            }
            /* Keys: one chord per bar */
            if (s == 0) {
                if (bar > 0) for (k = 0; k < 3; k++) genisys_engine_note_off(&keys, CHORDS[(bar - 1) % 4][k]);
                for (k = 0; k < 3; k++) genisys_engine_note_on(&keys, CHORDS[chord][k], 90);
            }
            /* Drums */
            if (s == 0 && bar % 4 == 0) genisys_engine_drum_hit(&drums, 49, 110);
            if (s == 0 || s == 8 || s == 10) genisys_engine_drum_hit(&drums, 36, 120);
            if (s == 4 || s == 12) genisys_engine_drum_hit(&drums, 38, 115);
            if (s % 2 == 0 && s != 14 && !(s == 0 && bar % 4 == 0)) genisys_engine_drum_hit(&drums, 42, s % 4 == 0 ? 100 : 75);
            if (s == 14) genisys_engine_drum_hit(&drums, 46, 90);
            if (bar == bars - 1 && s >= 12) genisys_engine_drum_hit(&drums, TOM_FILL[s - 12], 110);
        } else if (step == steps) {
            genisys_engine_all_notes_off(&bass);
            genisys_engine_all_notes_off(&keys);
            genisys_engine_drum_hit(&drums, 49, 120);
            genisys_engine_drum_hit(&drums, 36, 127);
            genisys_engine_note_on(&bass, 45, 120);
            for (k = 0; k < 3; k++) genisys_engine_note_on(&keys, CHORDS[0][k], 90);
        } else if (step == steps + 16) {
            genisys_engine_all_notes_off(&bass);
            genisys_engine_all_notes_off(&keys);
        }

        mix_in(&bass, left + offset, right + offset, STEP_FRAMES, 0.9f);
        mix_in(&keys, left + offset, right + offset, STEP_FRAMES, 0.8f);
        mix_in(&drums, left + offset, right + offset, STEP_FRAMES, 0.9f);
    }

    snprintf(path, sizeof path, "%s/genisys_demo.wav", dir);
    printf("%s %s\n", write_wav(path, left, right, frames) ? "wrote" : "FAILED to write", path);
    free(left);
    free(right);
}

/* ---- Chip model A/B ---- */

static void render_chip_compare(const char *dir) {
    static GenisysEngine e;
    static const int RIFF[16] = { 45, -1, 45, 57, -1, 45, 55, -1, 45, -1, 52, 53, -1, 45, 43, -1 };
    const int riff_frames = 16 * STEP_FRAMES * 2, gap = STEP_FRAMES * 8;
    const int frames = 3 * (riff_frames + gap);
    float *left = calloc((size_t)frames, sizeof(float));
    float *right = calloc((size_t)frames, sizeof(float));
    GenisysPatch patch = genisys_preset_patch(preset_index("Bass"));
    char path[1024];
    int model, rep, i;

    for (model = 0; model < 3; model++) {
        GenisysConsoleSettings console = genisys_default_console();
        int base = model * (riff_frames + gap), note = -1;
        console.chip_model = model;               /* YM2612, YM3438, Clean */
        console.filter_on = model < 2;            /* Clean also skips the console filter */
        genisys_engine_init(&e, RATE);
        genisys_engine_set_patch(&e, &patch);
        genisys_engine_set_console(&e, &console);

        for (rep = 0; rep < 2; rep++) {
            for (i = 0; i < 16; i++) {
                int offset = base + (rep * 16 + i) * STEP_FRAMES;
                if (RIFF[i] >= 0) {
                    if (note >= 0) genisys_engine_note_off(&e, note);
                    note = RIFF[i];
                    /* Second pass is quieter, where the ladder effect is most audible. */
                    genisys_engine_note_on(&e, note, rep == 0 ? 120 : 45);
                }
                mix_in(&e, left + offset, right + offset, STEP_FRAMES, 1.0f);
            }
        }
        genisys_engine_all_notes_off(&e);
        mix_in(&e, left + base + riff_frames, right + base + riff_frames, gap, 1.0f);
    }

    snprintf(path, sizeof path, "%s/genisys_chip_compare.wav", dir);
    printf("%s %s\n", write_wav(path, left, right, frames) ? "wrote" : "FAILED to write", path);
    free(left);
    free(right);
}

/* ---- Preset tour ---- */

#define TOUR_SECONDS_PER_PRESET 2.0

typedef struct { double at; int note; double length; } TourNote;

/* A short phrase that shows off a preset, picked by its category. */
static int tour_phrase(const char *category, TourNote *out) {
    int n = 0, i;
#define ADD(t, nt, len) do { out[n].at = (t); out[n].note = (nt); out[n].length = (len); n++; } while (0)
    if (strcmp(category, "Bass") == 0) {
        static const int RIFF[8] = { 40, 40, 52, 40, 43, 43, 55, 47 };
        for (i = 0; i < 8; i++) ADD(i * 0.18, RIFF[i], 0.14);
    } else if (strcmp(category, "Lead") == 0 || strcmp(category, "Brass & Winds") == 0) {
        static const int MELODY[5] = { 67, 69, 71, 74, 72 };
        for (i = 0; i < 4; i++) ADD(i * 0.22, MELODY[i], 0.2);
        ADD(0.88, MELODY[4], 0.8);
    } else if (strcmp(category, "Bells & Mallets") == 0 || strcmp(category, "Plucks") == 0) {
        static const int ARP[6] = { 60, 64, 67, 72, 67, 76 };
        for (i = 0; i < 6; i++) ADD(i * 0.2, ARP[i], 0.18);
    } else if (strcmp(category, "FM Percussion") == 0 || strcmp(category, "SFX") == 0) {
        ADD(0.0, 48, 0.4);
        ADD(0.5, 55, 0.4);
        ADD(1.0, 60, 0.6);
    } else if (strcmp(category, "Guitar") == 0) {
        ADD(0.0, 40, 0.5); ADD(0.0, 47, 0.5); ADD(0.0, 52, 0.5);
        ADD(0.6, 43, 0.9); ADD(0.6, 50, 0.9); ADD(0.6, 55, 0.9);
    } else {
        /* Keys, organs, pads, chip, init: a held chord. */
        ADD(0.0, 60, 1.5); ADD(0.0, 64, 1.5); ADD(0.0, 67, 1.5);
    }
#undef ADD
    return n;
}

static void render_preset_tour(const char *dir) {
    static GenisysEngine e;
    const int count = genisys_preset_count();
    const int per = (int)(RATE * TOUR_SECONDS_PER_PRESET);
    const int frames = count * per;
    float *left = calloc((size_t)frames, sizeof(float));
    float *right = calloc((size_t)frames, sizeof(float));
    char path[1024];
    FILE *index_file;
    int i;

    snprintf(path, sizeof path, "%s/genisys_preset_tour.txt", dir);
    index_file = fopen(path, "w");

    for (i = 0; i < count; i++) {
        TourNote notes[16];
        GenisysPatch patch = genisys_preset_patch(i);
        GenisysPsgSettings psg = genisys_preset_psg(i);
        int n = tour_phrase(genisys_preset_category(i), notes), k, pos = 0;
        int base = i * per;

        genisys_engine_init(&e, RATE);
        genisys_engine_set_patch(&e, &patch);
        genisys_engine_set_psg(&e, &psg);

        /* Step through the phrase in 10 ms slices, starting and stopping
         * notes on time. */
        while (pos < per) {
            int slice = RATE / 100;
            double t = (double)pos / RATE;
            for (k = 0; k < n; k++) {
                if (notes[k].at >= t && notes[k].at < t + 0.01) genisys_engine_note_on(&e, notes[k].note, 100);
                if (notes[k].at + notes[k].length >= t && notes[k].at + notes[k].length < t + 0.01) {
                    genisys_engine_note_off(&e, notes[k].note);
                }
            }
            if (pos + slice > per) slice = per - pos;
            mix_in(&e, left + base + pos, right + base + pos, slice, 1.0f);
            pos += slice;
        }
        if (index_file) {
            fprintf(index_file, "%5.1fs  %-18s %s\n", (double)base / RATE, genisys_preset_category(i), genisys_preset_name(i));
        }
    }
    if (index_file) fclose(index_file);

    snprintf(path, sizeof path, "%s/genisys_preset_tour.wav", dir);
    printf("%s %s\n", write_wav(path, left, right, frames) ? "wrote" : "FAILED to write", path);
    free(left);
    free(right);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    genisys_engine_global_init();
    render_demo(dir);
    render_chip_compare(dir);
    render_preset_tour(dir);
    return 0;
}

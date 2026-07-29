/* PC prototype harness: drives synth-core's PSG through the real register
 * write protocol, resamples its native-rate output down to the device
 * sample rate, and plays it through the speakers via miniaudio. This is
 * our accuracy-testing pipeline before there's any real UI. */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h> /* Sleep() -- PC-only, not part of synth-core */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "psg.h"
#include "ym2612.h"

#define OUTPUT_SAMPLE_RATE 48000

/* ---- Real SN76489 register-write helpers (build the actual latch/data
 * bytes a game would send, rather than poking chip state directly) ---- */

static void psg_set_tone(Psg *psg, int channel, double freq_hz) {
    uint16_t n = (uint16_t)(PSG_CLOCK_HZ / (32.0 * freq_hz) + 0.5);
    if (n > 0x3FF) n = 0x3FF;
    psg_write(psg, (uint8_t)(0x80 | (channel << 5) | (n & 0x0F)));
    psg_write(psg, (uint8_t)((n >> 4) & 0x3F));
}

static void psg_set_volume(Psg *psg, int channel, uint8_t attenuation) {
    psg_write(psg, (uint8_t)(0x80 | (channel << 5) | 0x10 | (attenuation & 0x0F)));
}

static void psg_set_noise(Psg *psg, uint8_t control) {
    psg_write(psg, (uint8_t)(0x80 | (3 << 5) | (control & 0x07)));
}

/* ---- Test sequence: an ascending C major scale on channel 0, then a
 * periodic-noise burst followed by a white-noise burst on channel 3 ---- */

typedef enum { EV_TONE, EV_VOLUME, EV_NOISE } EventType;

typedef struct {
    double at_sec;
    EventType type;
    int channel;
    double freq;      /* EV_TONE */
    uint8_t value;     /* EV_VOLUME (attenuation) / EV_NOISE (control) */
} SeqEvent;

#define SILENT 0x0F
#define AUDIBLE 0x04 /* moderate, comfortable listening volume */

static const double SCALE_HZ[8] = {
    261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25
};

static SeqEvent g_events[32];
static int g_num_events = 0;

static void build_sequence(void) {
    int i;
    double t = 0.0;
    const double note_dur = 0.25;

    g_events[g_num_events++] = (SeqEvent){ 0.0, EV_VOLUME, 0, 0, AUDIBLE };
    g_events[g_num_events++] = (SeqEvent){ 0.0, EV_VOLUME, 3, 0, SILENT };

    for (i = 0; i < 8; i++) {
        g_events[g_num_events++] = (SeqEvent){ t, EV_TONE, 0, SCALE_HZ[i], 0 };
        t += note_dur;
    }

    g_events[g_num_events++] = (SeqEvent){ t, EV_VOLUME, 0, 0, SILENT };

    t += 0.1;
    g_events[g_num_events++] = (SeqEvent){ t, EV_NOISE, 3, 0, 0x01 }; /* periodic, rate 1 */
    g_events[g_num_events++] = (SeqEvent){ t, EV_VOLUME, 3, 0, AUDIBLE };

    t += 0.5;
    g_events[g_num_events++] = (SeqEvent){ t, EV_NOISE, 3, 0, 0x05 }; /* white, rate 1 */

    t += 0.5;
    g_events[g_num_events++] = (SeqEvent){ t, EV_VOLUME, 3, 0, SILENT };
}

/* ---- YM2612 single-operator test: after the PSG sequence, play one FM
 * operator through a full attack/decay/sustain/release cycle so the log-
 * domain phase/envelope pipeline (milestone 2a) can be heard directly. ---- */

/* freq = ((fnum<<block)>>1) * YM2612_SAMPLE_HZ / (SIN_LEN<<SIN_BITS), for
 * MUL=1 (true x1) and DT=0 -- inverted to solve for fnum at a chosen block. */
static uint16_t ym_fnum_for_hz(double hz, int block) {
    double fnum = hz * 1048576.0 / (YM2612_SAMPLE_HZ * (double)(1 << (block - 1)));
    if (fnum < 0.0) fnum = 0.0;
    if (fnum > 2047.0) fnum = 2047.0;
    return (uint16_t)(fnum + 0.5);
}

static void setup_fm_test_patch(Ym2612Operator *op, double freq_hz) {
    const int block = 4;
    ym2612_operator_init(op);
    ym2612_operator_set_freq(op, ym_fnum_for_hz(freq_hz, block), (uint8_t)block);
    ym2612_operator_set_dt_mul(op, 0x01);   /* DT=0, MUL=1 (true x1) */
    ym2612_operator_set_tl(op, 0x05);       /* bright, comfortably audible */
    ym2612_operator_set_ar_ksr(op, 0x19);   /* AR=25: fast but visible attack ramp */
    ym2612_operator_set_d1r(op, 0x0A);      /* moderate decay down to sustain level */
    ym2612_operator_set_d2r(op, 0x02);      /* slow ongoing decay while held */
    ym2612_operator_set_sl_rr(op, 0x47);    /* SL=4, RR=7: moderate release */
}

/* ---- YM2612 full-channel test (milestone 2b): two independent 2-operator
 * FM pairs (algorithm 4) -- each pair is a modulator feeding a carrier,
 * which is the classic building block behind FM bell/electric-piano
 * patches. This is the point where it stops being "one sine tone" and
 * starts sounding like an actual FM instrument. ---- */

static void setup_fm_channel_patch(Ym2612Channel *ch, double freq_hz) {
    const int block = 4;
    uint16_t fnum = ym_fnum_for_hz(freq_hz, block);

    ym2612_channel_init(ch);
    ym2612_channel_set_algorithm(ch, 0x04); /* algo 4: (OP1>OP2) + (OP3>OP4), FB=0 */

    /* Pair 1: OP1 modulates OP2 (the carrier) */
    ym2612_operator_set_freq(&ch->op[0], fnum, (uint8_t)block);
    ym2612_operator_set_dt_mul(&ch->op[0], 0x01);  /* modulator at x1 */
    ym2612_operator_set_tl(&ch->op[0], 0x08);       /* moderate modulation depth */
    ym2612_operator_set_ar_ksr(&ch->op[0], 0x1F);
    ym2612_operator_set_d1r(&ch->op[0], 0x08);
    ym2612_operator_set_d2r(&ch->op[0], 0x02);
    ym2612_operator_set_sl_rr(&ch->op[0], 0x37);

    ym2612_operator_set_freq(&ch->op[1], fnum, (uint8_t)block);
    ym2612_operator_set_dt_mul(&ch->op[1], 0x01);  /* carrier at x1 (fundamental) */
    ym2612_operator_set_tl(&ch->op[1], 0x02);       /* loud: this is the main output */
    ym2612_operator_set_ar_ksr(&ch->op[1], 0x1B);
    ym2612_operator_set_d1r(&ch->op[1], 0x06);
    ym2612_operator_set_d2r(&ch->op[1], 0x02);
    ym2612_operator_set_sl_rr(&ch->op[1], 0x36);

    /* Pair 2: OP3 modulates OP4 (the carrier), an octave-ish up (MUL=2) and
     * quieter, layered in for a brighter, bell-like edge on the attack. */
    ym2612_operator_set_freq(&ch->op[2], fnum, (uint8_t)block);
    ym2612_operator_set_dt_mul(&ch->op[2], 0x02);  /* modulator at x2 */
    ym2612_operator_set_tl(&ch->op[2], 0x10);
    ym2612_operator_set_ar_ksr(&ch->op[2], 0x1F);
    ym2612_operator_set_d1r(&ch->op[2], 0x0C);
    ym2612_operator_set_d2r(&ch->op[2], 0x04);
    ym2612_operator_set_sl_rr(&ch->op[2], 0x38);

    ym2612_operator_set_freq(&ch->op[3], fnum, (uint8_t)block);
    ym2612_operator_set_dt_mul(&ch->op[3], 0x01);  /* carrier at x1 */
    ym2612_operator_set_tl(&ch->op[3], 0x0A);       /* quieter than the main carrier */
    ym2612_operator_set_ar_ksr(&ch->op[3], 0x1B);
    ym2612_operator_set_d1r(&ch->op[3], 0x08);
    ym2612_operator_set_d2r(&ch->op[3], 0x03);
    ym2612_operator_set_sl_rr(&ch->op[3], 0x37);
}

/* ---- Milestone 2c capstone: three channels playing a chord together,
 * driven entirely through the real register map/port addressing (the same
 * way a game would), plus a fourth channel demonstrating audible LFO
 * vibrato -- multiple independent voices and chip-wide modulation, both
 * actually exercised at once. ---- */

static void write_chip_freq(Ym2612Chip *chip, int port, int chan, double freq_hz, int block) {
    uint16_t fnum = ym_fnum_for_hz(freq_hz, block);
    ym2612_chip_write(chip, port, (uint8_t)(0xA4 + chan), (uint8_t)((block << 3) | ((fnum >> 8) & 7)));
    ym2612_chip_write(chip, port, (uint8_t)(0xA0 + chan), (uint8_t)(fnum & 0xFF));
}

/* Same 2-pair (algorithm 4) patch as setup_fm_channel_patch, but written
 * through the real register map -- note the physical slot offsets
 * (+0=OP1, +4=OP3, +8=OP2, +12=OP4), the well-documented scrambled order
 * every register-driven YM2612 write has to account for. */
static void write_chip_patch(Ym2612Chip *chip, int port, int chan, double freq_hz) {
    const int block = 4;

    ym2612_chip_write(chip, port, (uint8_t)(0xB0 + chan), 0x04); /* algo 4, FB=0 */

    ym2612_chip_write(chip, port, (uint8_t)(0x30 + chan), 0x01); /* OP1: DT/MUL */
    ym2612_chip_write(chip, port, (uint8_t)(0x40 + chan), 0x08); /* OP1: TL */
    ym2612_chip_write(chip, port, (uint8_t)(0x50 + chan), 0x1F); /* OP1: AR/KS */
    ym2612_chip_write(chip, port, (uint8_t)(0x60 + chan), 0x08); /* OP1: D1R */
    ym2612_chip_write(chip, port, (uint8_t)(0x70 + chan), 0x02); /* OP1: D2R */
    ym2612_chip_write(chip, port, (uint8_t)(0x80 + chan), 0x37); /* OP1: SL/RR */

    ym2612_chip_write(chip, port, (uint8_t)(0x34 + chan), 0x02); /* OP3: DT/MUL (x2) */
    ym2612_chip_write(chip, port, (uint8_t)(0x44 + chan), 0x10); /* OP3: TL */
    ym2612_chip_write(chip, port, (uint8_t)(0x54 + chan), 0x1F);
    ym2612_chip_write(chip, port, (uint8_t)(0x64 + chan), 0x0C);
    ym2612_chip_write(chip, port, (uint8_t)(0x74 + chan), 0x04);
    ym2612_chip_write(chip, port, (uint8_t)(0x84 + chan), 0x38);

    ym2612_chip_write(chip, port, (uint8_t)(0x38 + chan), 0x01); /* OP2: DT/MUL */
    ym2612_chip_write(chip, port, (uint8_t)(0x48 + chan), 0x02); /* OP2: TL (main carrier, loud) */
    ym2612_chip_write(chip, port, (uint8_t)(0x58 + chan), 0x1B);
    ym2612_chip_write(chip, port, (uint8_t)(0x68 + chan), 0x06);
    ym2612_chip_write(chip, port, (uint8_t)(0x78 + chan), 0x02);
    ym2612_chip_write(chip, port, (uint8_t)(0x88 + chan), 0x36);

    ym2612_chip_write(chip, port, (uint8_t)(0x3C + chan), 0x01); /* OP4: DT/MUL */
    ym2612_chip_write(chip, port, (uint8_t)(0x4C + chan), 0x0A); /* OP4: TL */
    ym2612_chip_write(chip, port, (uint8_t)(0x5C + chan), 0x1B);
    ym2612_chip_write(chip, port, (uint8_t)(0x6C + chan), 0x08);
    ym2612_chip_write(chip, port, (uint8_t)(0x7C + chan), 0x03);
    ym2612_chip_write(chip, port, (uint8_t)(0x8C + chan), 0x37);

    write_chip_freq(chip, port, chan, freq_hz, block);
}

/* op_on_bits: bit0=OP1 .. bit3=OP4 (all 0 = key everything off). */
static void chip_key(Ym2612Chip *chip, int global_chan, uint8_t op_on_bits) {
    uint8_t data = (uint8_t)((global_chan % 3) |
                              (global_chan >= 3 ? 0x04 : 0x00) |
                              (op_on_bits << 4));
    ym2612_chip_write(chip, 0, 0x28, data);
}

static const double CHORD_HZ[3] = { 261.63, 329.63, 392.00 }; /* C4, E4, G4 */

/* ---- Playback state ---- */

typedef struct {
    Psg psg;
    double elapsed_sec;
    double tick_accum;
    double ticks_per_sample;
    int event_cursor;

    Ym2612Operator fm_op;
    double fm_tick_accum;
    double fm_ticks_per_sample;
    double fm_key_on_at;
    double fm_key_off_at;
    int fm_keyed_on;
    int fm_keyed_off;

    Ym2612Channel fm_channel;
    double fm_ch_key_on_at;
    double fm_ch_key_off_at;
    int fm_ch_keyed_on;
    int fm_ch_keyed_off;

    Ym2612Chip chip;
    double chord_key_on_at;
    double chord_key_off_at;
    double vibrato_key_on_at;
    double vibrato_key_off_at;
    int chord_keyed_on;
    int chord_keyed_off;
    int vibrato_keyed_on;
    int vibrato_keyed_off;
} AppState;

static AppState g_app;

static void apply_event(Psg *psg, const SeqEvent *ev) {
    switch (ev->type) {
        case EV_TONE:   psg_set_tone(psg, ev->channel, ev->freq); break;
        case EV_VOLUME: psg_set_volume(psg, ev->channel, ev->value); break;
        case EV_NOISE:  psg_set_noise(psg, ev->value); break;
    }
}

static void data_callback(ma_device *device, void *output, const void *input, ma_uint32 frame_count) {
    int16_t *out = (int16_t *)output;
    ma_uint32 i;
    (void)input;
    (void)device;

    for (i = 0; i < frame_count; i++) {
        int32_t psg_sum = 0;
        int32_t fm_mono_sum = 0;  /* fm_op + fm_channel: no per-channel pan, applied equally to both sides */
        int32_t fm_left_sum = 0, fm_right_sum = 0; /* g_app.chip: true per-channel L/R pan */
        int psg_n, fm_n, k;

        while (g_app.event_cursor < g_num_events &&
               g_events[g_app.event_cursor].at_sec <= g_app.elapsed_sec) {
            apply_event(&g_app.psg, &g_events[g_app.event_cursor]);
            g_app.event_cursor++;
        }

        if (!g_app.fm_keyed_on && g_app.elapsed_sec >= g_app.fm_key_on_at) {
            ym2612_operator_key_on(&g_app.fm_op);
            g_app.fm_keyed_on = 1;
        }
        if (!g_app.fm_keyed_off && g_app.elapsed_sec >= g_app.fm_key_off_at) {
            ym2612_operator_key_off(&g_app.fm_op);
            g_app.fm_keyed_off = 1;
        }
        if (!g_app.fm_ch_keyed_on && g_app.elapsed_sec >= g_app.fm_ch_key_on_at) {
            ym2612_channel_key_on(&g_app.fm_channel, 0x0F);
            g_app.fm_ch_keyed_on = 1;
        }
        if (!g_app.fm_ch_keyed_off && g_app.elapsed_sec >= g_app.fm_ch_key_off_at) {
            ym2612_channel_key_off(&g_app.fm_channel, 0x0F);
            g_app.fm_ch_keyed_off = 1;
        }
        if (!g_app.chord_keyed_on && g_app.elapsed_sec >= g_app.chord_key_on_at) {
            int c;
            for (c = 0; c < 3; c++) chip_key(&g_app.chip, c, 0x0F);
            g_app.chord_keyed_on = 1;
        }
        if (!g_app.chord_keyed_off && g_app.elapsed_sec >= g_app.chord_key_off_at) {
            int c;
            for (c = 0; c < 3; c++) chip_key(&g_app.chip, c, 0x00);
            g_app.chord_keyed_off = 1;
        }
        if (!g_app.vibrato_keyed_on && g_app.elapsed_sec >= g_app.vibrato_key_on_at) {
            chip_key(&g_app.chip, 3, 0x0F);
            g_app.vibrato_keyed_on = 1;
        }
        if (!g_app.vibrato_keyed_off && g_app.elapsed_sec >= g_app.vibrato_key_off_at) {
            chip_key(&g_app.chip, 3, 0x00);
            g_app.vibrato_keyed_off = 1;
        }

        /* Box-filter decimation from each chip's native tick rate (PSG
         * ~223.7kHz, YM2612 ~53.267kHz) down to the output rate: average
         * every internal tick within this output sample's period rather
         * than naively picking one (nearest-neighbor would alias badly). */
        g_app.tick_accum += g_app.ticks_per_sample;
        psg_n = (int)g_app.tick_accum;
        if (psg_n < 1) psg_n = 1;
        g_app.tick_accum -= psg_n;
        for (k = 0; k < psg_n; k++) {
            psg_sum += psg_clock(&g_app.psg);
        }

        g_app.fm_tick_accum += g_app.fm_ticks_per_sample;
        fm_n = (int)g_app.fm_tick_accum;
        if (fm_n < 1) fm_n = 1;
        g_app.fm_tick_accum -= fm_n;
        for (k = 0; k < fm_n; k++) {
            sample_t chip_l, chip_r;
            fm_mono_sum += ym2612_operator_clock(&g_app.fm_op);
            fm_mono_sum += ym2612_channel_clock(&g_app.fm_channel, 0, 0);
            ym2612_chip_clock(&g_app.chip, &chip_l, &chip_r);
            fm_left_sum += chip_l;
            fm_right_sum += chip_r;
        }

        {
            int32_t centered = (psg_sum / psg_n) + (fm_mono_sum / fm_n);
            int16_t left = clamp_s16(centered + (fm_left_sum / fm_n));
            int16_t right = clamp_s16(centered + (fm_right_sum / fm_n));
            out[i * 2 + 0] = left;
            out[i * 2 + 1] = right;
        }

        g_app.elapsed_sec += 1.0 / OUTPUT_SAMPLE_RATE;
    }
}

int main(void) {
    ma_device_config config;
    ma_device device;

    psg_reset(&g_app.psg);
    g_app.elapsed_sec = 0.0;
    g_app.tick_accum = 0.0;
    g_app.ticks_per_sample = PSG_TICK_HZ / OUTPUT_SAMPLE_RATE;
    g_app.event_cursor = 0;
    build_sequence();

    setup_fm_test_patch(&g_app.fm_op, 440.0); /* A4 */
    g_app.fm_tick_accum = 0.0;
    g_app.fm_ticks_per_sample = YM2612_SAMPLE_HZ / OUTPUT_SAMPLE_RATE;
    g_app.fm_key_on_at = 3.4;
    g_app.fm_key_off_at = 4.6;
    g_app.fm_keyed_on = 0;
    g_app.fm_keyed_off = 0;

    setup_fm_channel_patch(&g_app.fm_channel, 220.0); /* A3 */
    g_app.fm_ch_key_on_at = 5.9;
    g_app.fm_ch_key_off_at = 7.1;
    g_app.fm_ch_keyed_on = 0;
    g_app.fm_ch_keyed_off = 0;

    ym2612_chip_init(&g_app.chip);
    ym2612_chip_write(&g_app.chip, 0, 0x22, 0x0B); /* LFO on, moderate rate */
    {
        /* Milestone 2d: spread the chord across the stereo field using the
         * real pan bits -- C4 hard left, E4 hard right, G4 centered. */
        static const uint8_t CHORD_PAN[3] = { 0x80, 0x40, 0xC0 };
        int c;
        for (c = 0; c < 3; c++) {
            write_chip_patch(&g_app.chip, 0, c, CHORD_HZ[c]);
            ym2612_chip_write(&g_app.chip, 0, (uint8_t)(0xB4 + c), CHORD_PAN[c]);
        }
    }
    write_chip_patch(&g_app.chip, 1, 0, 220.0);     /* channel index 3 (A3), for vibrato */
    ym2612_chip_write(&g_app.chip, 1, 0xB4, 0xC7);  /* pan L+R on, PMS = 7 (max) on that channel */
    g_app.chord_key_on_at = 8.8;
    g_app.chord_key_off_at = 10.3;
    g_app.vibrato_key_on_at = 10.8;
    g_app.vibrato_key_off_at = 12.8;
    g_app.chord_keyed_on = 0;
    g_app.chord_keyed_off = 0;
    g_app.vibrato_keyed_on = 0;
    g_app.vibrato_keyed_off = 0;

    config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate = OUTPUT_SAMPLE_RATE;
    config.dataCallback = data_callback;

    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize audio device\n");
        return 1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start audio device\n");
        ma_device_uninit(&device);
        return 1;
    }

    printf("Playing SN76489 PSG test sequence, a YM2612 single-operator note, ");
    printf("a full 4-operator FM channel patch, a 3-channel chord, ");
    printf("then a channel with audible LFO vibrato...\n");
    Sleep(14000);

    ma_device_uninit(&device);
    return 0;
}

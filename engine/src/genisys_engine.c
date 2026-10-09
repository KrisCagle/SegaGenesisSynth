#include "genisys_engine.h"

#include <math.h>
#include <string.h>

/* Output scaling: one full-volume FM channel peaks at 8191, which maps to
 * 0.25 here -- the same loudness the desktop app always had. Several loud
 * voices can exceed 1.0; that's deliberate (see genisys_engine_render). */
#define OUTPUT_SCALE (1.0f / 32768.0f)

/* YM2612 register layout quirk: operator slots are ordered OP1, OP3, OP2,
 * OP4 in the register map, so logical OP1..OP4 sit at these offsets. */
static const int OP_REG_OFFSET[4] = { 0, 8, 4, 12 };

/* Velocity -> extra attenuation in TL steps (0.75 dB each), at 100%
 * sensitivity. Uses the common MIDI convention amplitude ~ (vel/127)^2,
 * i.e. 40*log10(127/vel) dB. Built in genisys_engine_global_init(). */
static float g_velocity_att[128];

void genisys_engine_global_init(void) {
    int v;
    ym2612_init_tables();
    g_velocity_att[0] = 127.0f;
    for (v = 1; v < 128; v++) {
        g_velocity_att[v] = (float)(40.0 * log10(127.0 / v) / 0.75);
    }
}

uint8_t genisys_carrier_mask(int algorithm) {
    static const uint8_t CARRIERS[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };
    return CARRIERS[algorithm & 7];
}

void genisys_note_to_block_fnum(int note, int *block, int *fnum) {
    double hz = 440.0 * pow(2.0, (note - 69) / 12.0);
    double f;
    int b;

    /* Sound drivers keep each octave's C near fnum 644 in block = octave
     * (MIDI 60 = C4 -> block 4). Staying in that range also keeps the
     * chip's key code, which drives detune and key scaling, matching how
     * real game music behaves. The old fixed block 4 ran out of fnum range
     * above ~832 Hz, so every note from A5 up played the same pitch. */
    b = note / 12 - 1;
    if (b < 0) b = 0;
    if (b > 7) b = 7;
    f = hz * 1048576.0 / (YM2612_SAMPLE_HZ * ldexp(1.0, b - 1));
    while (f > 2047.0 && b < 7) {
        b++;
        f = hz * 1048576.0 / (YM2612_SAMPLE_HZ * ldexp(1.0, b - 1));
    }
    if (f > 2047.0) f = 2047.0; /* above ~6.6 kHz: the chip's ceiling */

    *block = b;
    *fnum = (int)(f + 0.5);
}

GenisysPatch genisys_default_patch(void) {
    static const int TL[4] = { 22, 26, 30, 5 };
    GenisysPatch p;
    int i;

    memset(&p, 0, sizeof p);
    p.lfo_rate = 3;
    p.velocity_sens = 50;
    for (i = 0; i < 4; i++) {
        GenisysOperatorParams *o = &p.op[i];
        o->mul = 1;
        o->dt = 4;
        o->tl = TL[i];
        o->ar = 31;
        o->d1r = 10;
        o->sl = 4;
        o->rr = 8;
    }
    return p;
}

/* ---- Register writes ---- */

static void chip_write_channel(GenisysEngine *e, int voice, uint8_t base_reg, uint8_t data) {
    ym2612_chip_write(&e->chip, voice / 3, (uint8_t)(base_reg + voice % 3), data);
}

static int voice_tl(const GenisysEngine *e, int voice, int op) {
    const GenisysPatch *p = &e->patch;
    int tl = p->op[op].tl;

    if (genisys_carrier_mask(p->algorithm) & (1 << op)) {
        int vel = e->voice[voice].velocity;
        if (vel < 1) vel = 127;
        tl += (int)(g_velocity_att[vel] * (float)p->velocity_sens / 100.0f + 0.5f);
    }
    return tl > 127 ? 127 : tl;
}

/* TL is the only per-voice register (it carries the velocity), so it is
 * written separately on every note-on as well as on patch changes. */
static void write_voice_tl(GenisysEngine *e, int voice) {
    int op;
    for (op = 0; op < 4; op++) {
        chip_write_channel(e, voice, (uint8_t)(0x40 + OP_REG_OFFSET[op]), (uint8_t)voice_tl(e, voice, op));
    }
}

static void write_voice_patch(GenisysEngine *e, int voice) {
    const GenisysPatch *p = &e->patch;
    int op;

    chip_write_channel(e, voice, 0xB0, (uint8_t)((p->algorithm & 7) | ((p->feedback & 7) << 3)));
    for (op = 0; op < 4; op++) {
        const GenisysOperatorParams *o = &p->op[op];
        uint8_t off = (uint8_t)OP_REG_OFFSET[op];
        chip_write_channel(e, voice, (uint8_t)(0x30 + off), (uint8_t)(((o->dt & 7) << 4) | (o->mul & 0x0F)));
        chip_write_channel(e, voice, (uint8_t)(0x50 + off), (uint8_t)(((o->ks & 3) << 6) | (o->ar & 0x1F)));
        chip_write_channel(e, voice, (uint8_t)(0x60 + off), (uint8_t)((o->am ? 0x80 : 0) | (o->d1r & 0x1F)));
        chip_write_channel(e, voice, (uint8_t)(0x70 + off), (uint8_t)(o->d2r & 0x1F));
        chip_write_channel(e, voice, (uint8_t)(0x80 + off), (uint8_t)(((o->sl & 0x0F) << 4) | (o->rr & 0x0F)));
        chip_write_channel(e, voice, (uint8_t)(0x90 + off), (uint8_t)((o->ssg_enable ? 0x08 : 0) | (o->ssg_mode & 7)));
    }
    write_voice_tl(e, voice);
    /* Both speakers on (0xC0), plus the LFO sensitivities. */
    chip_write_channel(e, voice, 0xB4, (uint8_t)(0xC0 | ((p->ams & 3) << 4) | (p->pms & 7)));
}

static void key(GenisysEngine *e, int voice, int on) {
    uint8_t chan_bits = (uint8_t)((voice % 3) | (voice >= 3 ? 0x04 : 0x00));
    ym2612_chip_write(&e->chip, 0, 0x28, (uint8_t)(chan_bits | (on ? 0xF0 : 0x00)));
}

/* ---- PSG helpers (the real SN76489 latch/data byte protocol) ---- */

static void psg_tone(GenisysEngine *e, int channel, double hz) {
    int n = (int)(PSG_CLOCK_HZ / (32.0 * hz) + 0.5);
    if (n > 0x3FF) n = 0x3FF;
    if (n < 1) n = 1;
    psg_write(&e->psg, (uint8_t)(0x80 | (channel << 5) | (n & 0x0F)));
    psg_write(&e->psg, (uint8_t)((n >> 4) & 0x3F));
}

static void psg_volume(GenisysEngine *e, int channel, int attenuation) {
    psg_write(&e->psg, (uint8_t)(0x80 | (channel << 5) | 0x10 | (attenuation & 0x0F)));
}

static void apply_psg_noise(GenisysEngine *e) {
    const GenisysPsgSettings *s = &e->psg_settings;
    psg_write(&e->psg, (uint8_t)(0x80 | (3 << 5) | (s->noise_white ? 0x04 : 0) | (s->noise_rate & 3)));
    psg_volume(e, 3, s->noise_on ? 15 - s->noise_volume : 15);
}

/* ---- Lifecycle ---- */

void genisys_engine_init(GenisysEngine *e, double sample_rate) {
    int v;
    memset(e, 0, sizeof *e);
    ym2612_chip_init(&e->chip);
    psg_reset(&e->psg);
    e->psg_ticks_per_fm_sample = PSG_TICK_HZ / YM2612_SAMPLE_HZ;
    e->patch = genisys_default_patch();
    e->psg_settings.noise_white = 1;
    e->psg_settings.noise_rate = 1;
    e->psg_settings.noise_volume = 10;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        e->voice[v].state = GENISYS_VOICE_FREE;
        e->voice[v].velocity = 127;
    }
    genisys_engine_set_patch(e, &e->patch);
    apply_psg_noise(e);
    genisys_engine_set_sample_rate(e, sample_rate);
}

void genisys_engine_set_sample_rate(GenisysEngine *e, double sample_rate) {
    e->sample_rate = sample_rate;
    genisys_resampler_init(&e->resampler, YM2612_SAMPLE_HZ, sample_rate);
}

void genisys_engine_set_patch(GenisysEngine *e, const GenisysPatch *patch) {
    int v;
    e->patch = *patch;
    ym2612_chip_write(&e->chip, 0, 0x22, (uint8_t)((patch->lfo_enable ? 0x08 : 0) | (patch->lfo_rate & 7)));
    for (v = 0; v < GENISYS_NUM_VOICES; v++) write_voice_patch(e, v);
}

void genisys_engine_set_psg(GenisysEngine *e, const GenisysPsgSettings *settings) {
    int v;
    e->psg_settings = *settings;
    apply_psg_noise(e);
    /* Re-level PSG doubles that are already sounding, so dragging the level
     * mid-note takes effect immediately. */
    for (v = 0; v < 3; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) {
            psg_volume(e, v, settings->level > 0 ? 15 - settings->level : 15);
        }
    }
}

/* ---- Voice allocation ---- */

static int voice_is_silent(const GenisysEngine *e, int voice) {
    uint8_t carriers = genisys_carrier_mask(e->patch.algorithm);
    int op;
    for (op = 0; op < 4; op++) {
        if ((carriers & (1 << op)) && e->chip.channel[voice].op[op].state != YM_EG_OFF) return 0;
    }
    return 1;
}

static int pick_oldest(const GenisysEngine *e, GenisysVoiceState state) {
    int v, best = -1;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == state && (best < 0 || e->voice[v].age < e->voice[best].age)) best = v;
    }
    return best;
}

/* Preference order: retrigger the same note; else a silent voice; else the
 * voice released longest ago (its tail is the quietest); else steal the
 * oldest held note. Taking the *oldest* free voice rather than the lowest
 * numbered one means a release tail is never cut off while another voice
 * is available. */
static int allocate_voice(GenisysEngine *e, int note) {
    int v;

    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state != GENISYS_VOICE_FREE && e->voice[v].note == note) return v;
    }
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_RELEASED && voice_is_silent(e, v)) {
            e->voice[v].state = GENISYS_VOICE_FREE;
        }
    }
    v = pick_oldest(e, GENISYS_VOICE_FREE);
    if (v < 0) v = pick_oldest(e, GENISYS_VOICE_RELEASED);
    if (v < 0) v = pick_oldest(e, GENISYS_VOICE_HELD);
    return v;
}

void genisys_engine_note_on(GenisysEngine *e, int note, int velocity) {
    int v, block, fnum;

    if (note < 0 || note > 127) return;
    if (velocity <= 0) { /* MIDI convention: note-on with velocity 0 is a note-off */
        genisys_engine_note_off(e, note);
        return;
    }
    if (velocity > 127) velocity = 127;

    v = allocate_voice(e, note);
    key(e, v, 0); /* the chip only restarts the envelope on an off->on edge */

    e->voice[v].state = GENISYS_VOICE_HELD;
    e->voice[v].note = note;
    e->voice[v].velocity = velocity;
    e->voice[v].age = ++e->event_counter;

    genisys_note_to_block_fnum(note, &block, &fnum);
    /* $A4 (block + fnum high bits) must be written before $A0 (fnum low),
     * which latches both. */
    chip_write_channel(e, v, 0xA4, (uint8_t)((block << 3) | ((fnum >> 8) & 7)));
    chip_write_channel(e, v, 0xA0, (uint8_t)(fnum & 0xFF));
    write_voice_tl(e, v);
    key(e, v, 1);

    if (v < 3) {
        if (e->psg_settings.level > 0) {
            psg_tone(e, v, 440.0 * pow(2.0, (note - 69) / 12.0));
            psg_volume(e, v, 15 - e->psg_settings.level);
        } else {
            psg_volume(e, v, 15);
        }
    }
}

void genisys_engine_note_off(GenisysEngine *e, int note) {
    int v;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD && e->voice[v].note == note) {
            key(e, v, 0);
            e->voice[v].state = GENISYS_VOICE_RELEASED;
            e->voice[v].age = ++e->event_counter;
            if (v < 3) psg_volume(e, v, 15);
        }
    }
}

void genisys_engine_all_notes_off(GenisysEngine *e) {
    int v;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) genisys_engine_note_off(e, e->voice[v].note);
    }
}

/* ---- Rendering ---- */

/* One native-rate (~53 kHz) sample of FM + PSG. The PSG ticks ~4.2 times
 * per FM sample; averaging those ticks is the area under its square waves
 * for that sample period, which is the right way to sample a signal that
 * only changes at tick boundaries. */
static void clock_native(GenisysEngine *e, float *left, float *right) {
    int32_t fm_l, fm_r, psg_sum = 0;
    int n, k;

    ym2612_chip_clock_wide(&e->chip, &fm_l, &fm_r);

    e->psg_tick_accum += e->psg_ticks_per_fm_sample;
    n = (int)e->psg_tick_accum;
    e->psg_tick_accum -= n;
    for (k = 0; k < n; k++) psg_sum += psg_clock(&e->psg);
    if (n > 0) psg_sum /= n;

    *left = (float)(fm_l + psg_sum) * OUTPUT_SCALE;
    *right = (float)(fm_r + psg_sum) * OUTPUT_SCALE;
}

void genisys_engine_render(GenisysEngine *e, float *out_left, float *out_right, int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        int need = genisys_resampler_needed(&e->resampler);
        while (need-- > 0) {
            float l, r;
            clock_native(e, &l, &r);
            genisys_resampler_push(&e->resampler, l, r);
        }
        genisys_resampler_read(&e->resampler, &out_left[i], &out_right[i]);
    }
}

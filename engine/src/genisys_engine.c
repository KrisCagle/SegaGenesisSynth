#include "genisys_engine.h"
#include "engine_internal.h"

#include <math.h>
#include <string.h>

/* Output scaling: one full-volume FM channel peaks at 8191, which maps to
 * 0.25 here -- the same loudness the desktop app always had. Several loud
 * voices can exceed 1.0; that's deliberate (see genisys_engine_render). */
#define OUTPUT_SCALE (1.0f / 32768.0f)

/* FM/PSG balance. synth-core's PSG swings +/-828 at full volume while one
 * full FM channel peaks at 8191. Genesis Plus GX, whose mix was tuned
 * against hardware, plays a full PSG square as 0..2800 at a 150% preamp,
 * i.e. +/-2100 against the same 8191 FM peak. Match that. */
#define PSG_MIX_GAIN (2100.0f / 828.0f)

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
    genisys_drums_init();
    g_velocity_att[0] = 127.0f;
    for (v = 1; v < 128; v++) {
        g_velocity_att[v] = (float)(40.0 * log10(127.0 / v) / 0.75);
    }
}

float engine_velocity_db(int velocity) {
    if (velocity < 1) return 120.0f;
    if (velocity > 127) velocity = 127;
    return 0.75f * g_velocity_att[velocity];
}

float engine_velocity_gain(int velocity) {
    if (velocity < 1) return 0.0f;
    return (float)pow(10.0, -engine_velocity_db(velocity) / 20.0);
}

int engine_db_to_psg_steps(float db) {
    if (db <= 0.0f) return 0;
    return (int)(db / 2.0f + 0.5f);
}

uint8_t genisys_carrier_mask(int algorithm) {
    static const uint8_t CARRIERS[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };
    return CARRIERS[algorithm & 7];
}

void genisys_note_to_block_fnum(int note, int *block, int *fnum) {
    genisys_pitch_to_block_fnum((double)note, block, fnum);
}

void genisys_pitch_to_block_fnum(double pitch, int *block, int *fnum) {
    double hz = 440.0 * pow(2.0, (pitch - 69.0) / 12.0);
    int note = (int)floor(pitch + 0.5);
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
    /* A deep downward bend: drop a block rather than lose fnum precision. */
    while (f < 600.0 && b > 0) {
        b--;
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
    p.vibrato_depth = 50;
    p.vibrato_rate = 55;
    p.voice_mode = GENISYS_MODE_POLY;
    p.glide_time = 0;
    p.unison = 1;
    p.unison_detune = 12;
    p.unison_stereo = 1;
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

GenisysConsoleSettings genisys_default_console(void) {
    GenisysConsoleSettings c;
    c.chip_model = GENISYS_CHIP_YM2612;
    c.filter_on = 1;
    c.filter_hz = GENISYS_MODEL1_FILTER_HZ;
    return c;
}

/* ---- Register writes ---- */

static void chip_write_channel(GenisysEngine *e, int voice, uint8_t base_reg, uint8_t data) {
    ym2612_chip_write(&e->chip, voice / 3, (uint8_t)(base_reg + voice % 3), data);
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Quick Sound macro offsets, in register steps. Scaled so the full -100..100
 * range covers what's musically useful for each register. */
static int macro_steps(int amount, int max_steps) {
    return (int)(clamp_int(amount, -100, 100) * max_steps / 100.0 + (amount >= 0 ? 0.5 : -0.5));
}

static int voice_tl(const GenisysEngine *e, int voice, int op) {
    const GenisysPatch *p = &e->patch;
    int tl = p->op[op].tl;

    if (!(genisys_carrier_mask(p->algorithm) & (1 << op))) {
        /* BRIGHT: modulators only, so it changes tone, not volume. A silent
         * (127) modulator stays silent. */
        if (tl < 127) tl = clamp_int(tl - macro_steps(p->macro_bright, 40), 0, 126);
    } else {
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

    /* Macros adjust rates: higher register rates are faster, so "longer"
     * means subtracting. Attack/release never reach 0 (which would mean
     * "never"), and decays that are 0 (sustaining) stay 0. */
    int atk = macro_steps(p->macro_attack, 20);
    int dec = macro_steps(p->macro_decay, 16);
    int rel = macro_steps(p->macro_release, 10);

    chip_write_channel(e, voice, 0xB0, (uint8_t)((p->algorithm & 7) | ((p->feedback & 7) << 3)));
    for (op = 0; op < 4; op++) {
        const GenisysOperatorParams *o = &p->op[op];
        uint8_t off = (uint8_t)OP_REG_OFFSET[op];
        int ar = o->ar > 0 ? clamp_int(o->ar - atk, 1, 31) : 0;
        int d1r = o->d1r > 0 ? clamp_int(o->d1r - dec, 1, 31) : 0;
        int d2r = o->d2r > 0 ? clamp_int(o->d2r - dec, 1, 31) : 0;
        int rr = clamp_int(o->rr - rel, 1, 15);
        chip_write_channel(e, voice, (uint8_t)(0x30 + off), (uint8_t)(((o->dt & 7) << 4) | (o->mul & 0x0F)));
        chip_write_channel(e, voice, (uint8_t)(0x50 + off), (uint8_t)(((o->ks & 3) << 6) | ar));
        chip_write_channel(e, voice, (uint8_t)(0x60 + off), (uint8_t)((o->am ? 0x80 : 0) | d1r));
        chip_write_channel(e, voice, (uint8_t)(0x70 + off), (uint8_t)d2r);
        chip_write_channel(e, voice, (uint8_t)(0x80 + off), (uint8_t)(((o->sl & 0x0F) << 4) | rr));
        chip_write_channel(e, voice, (uint8_t)(0x90 + off), (uint8_t)((o->ssg_enable ? 0x08 : 0) | (o->ssg_mode & 7)));
    }
    write_voice_tl(e, voice);
    /* Both speakers on (0xC0), plus the LFO sensitivities. */
    chip_write_channel(e, voice, 0xB4, (uint8_t)(e->voice[voice].pan | ((p->ams & 3) << 4) | (p->pms & 7)));
}

static int is_mono(const GenisysEngine *e) {
    return e->patch.voice_mode == GENISYS_MODE_MONO || e->patch.voice_mode == GENISYS_MODE_LEGATO;
}

static int unison_count(const GenisysEngine *e) {
    int n = e->patch.unison;
    return n < 1 ? 1 : (n > 3 ? 3 : n);
}

static void write_voice_pitch(GenisysEngine *e, int v) {
    int block, fnum;
    double base = is_mono(e) ? e->mono_pitch : (double)e->voice[v].note;
    genisys_pitch_to_block_fnum(base + e->voice[v].detune + engine_pitch_offset(e), &block, &fnum);
    if (block == e->voice[v].last_block && fnum == e->voice[v].last_fnum) return;
    /* $A4 (block + fnum high bits) must be written before $A0 (fnum low),
     * which latches both. */
    chip_write_channel(e, v, 0xA4, (uint8_t)((block << 3) | ((fnum >> 8) & 7)));
    chip_write_channel(e, v, 0xA0, (uint8_t)(fnum & 0xFF));
    e->voice[v].last_block = block;
    e->voice[v].last_fnum = fnum;
}

double engine_pitch_offset(const GenisysEngine *e) {
    /* The VIBRATO macro sets a floor; the mod wheel can add more. */
    double amount = e->patch.vibrato_amount / 100.0;
    double vibrato = 0.0;
    if (e->mod_wheel > amount) amount = e->mod_wheel;
    if (amount > 0.0 && e->patch.vibrato_depth > 0) {
        vibrato = amount * e->patch.vibrato_depth / 100.0 * sin(2.0 * 3.14159265358979323846 * e->vibrato_phase);
    }
    return e->bend_semitones + vibrato;
}

uint8_t genisys_engine_active_voices(const GenisysEngine *e) {
    uint8_t mask = 0;
    int v;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) {
            mask |= (uint8_t)(1 << v);
        } else if (e->voice[v].state == GENISYS_VOICE_RELEASED) {
            /* Still audible if any carrier's envelope is running. */
            uint8_t carriers = genisys_carrier_mask(e->patch.algorithm);
            int op;
            for (op = 0; op < 4; op++) {
                if ((carriers & (1 << op)) && e->chip.channel[v].op[op].state != YM_EG_OFF) {
                    mask |= (uint8_t)(1 << v);
                    break;
                }
            }
        }
    }
    return mask;
}

static void key(GenisysEngine *e, int voice, int on) {
    uint8_t chan_bits = (uint8_t)((voice % 3) | (voice >= 3 ? 0x04 : 0x00));
    ym2612_chip_write(&e->chip, 0, 0x28, (uint8_t)(chan_bits | (on ? 0xF0 : 0x00)));
}

/* ---- Lifecycle ---- */

void genisys_engine_init(GenisysEngine *e, double sample_rate) {
    int v;
    memset(e, 0, sizeof *e);
    ym2612_chip_init(&e->chip);
    psg_reset(&e->psg);
    e->psg_ticks_per_fm_sample = PSG_TICK_HZ / YM2612_SAMPLE_HZ;
    e->patch = genisys_default_patch();
    e->psg_settings = genisys_default_psg();
    e->drum_settings.enabled = 0;
    e->drum_settings.level = 80;
    e->dac_last_written = 0x80;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        e->voice[v].state = GENISYS_VOICE_FREE;
        e->voice[v].velocity = 127;
        e->voice[v].last_block = -1;
        e->voice[v].last_fnum = -1;
        e->voice[v].pan = 0xC0;
    }
    genisys_engine_set_patch(e, &e->patch);
    engine_psg_reset(e);
    e->dc_coeff = (float)exp(-2.0 * 3.14159265358979323846 * 5.0 / YM2612_SAMPLE_HZ);
    {
        GenisysConsoleSettings c = genisys_default_console();
        genisys_engine_set_console(e, &c);
    }
    genisys_engine_set_sample_rate(e, sample_rate);
}

void genisys_engine_set_console(GenisysEngine *e, const GenisysConsoleSettings *settings) {
    static const Ym2612DacMode DAC_MODE[3] = { YM2612_DAC_YM2612, YM2612_DAC_YM3438, YM2612_DAC_CLEAN };
    double hz = settings->filter_hz;
    int model = settings->chip_model;

    if (model < 0 || model > 2) model = GENISYS_CHIP_YM2612;
    if (model != e->console.chip_model || !e->dc_primed) {
        /* The ladder effect's constant offset changes with the model: let the
         * DC blocker settle on the new level instead of producing a thump. */
        e->dc_primed = 0;
    }
    e->console = *settings;
    e->console.chip_model = model;
    ym2612_chip_set_dac_mode(&e->chip, DAC_MODE[model]);

    if (hz < 20.0) hz = 20.0;
    if (hz > YM2612_SAMPLE_HZ * 0.45) hz = YM2612_SAMPLE_HZ * 0.45;
    e->lp_coeff = (float)exp(-2.0 * 3.14159265358979323846 * hz / YM2612_SAMPLE_HZ);
}

void genisys_engine_set_sample_rate(GenisysEngine *e, double sample_rate) {
    e->sample_rate = sample_rate;
    genisys_resampler_init(&e->resampler, YM2612_SAMPLE_HZ, sample_rate);
}

static void release_all_voices(GenisysEngine *e) {
    int v;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) {
            key(e, v, 0);
            e->voice[v].state = GENISYS_VOICE_RELEASED;
            e->voice[v].sustained = 0;
            e->voice[v].age = ++e->event_counter;
        }
        e->voice[v].detune = 0.0;
        e->voice[v].pan = 0xC0;
    }
    e->stack_count = 0;
    engine_psg_all_notes_off(e);
}

void genisys_engine_set_patch(GenisysEngine *e, const GenisysPatch *patch) {
    int v;
    if (patch->voice_mode != e->patch.voice_mode || patch->unison != e->patch.unison) {
        /* A different way of using the channels: start clean. */
        release_all_voices(e);
    }
    e->patch = *patch;
    ym2612_chip_write(&e->chip, 0, 0x22, (uint8_t)((patch->lfo_enable ? 0x08 : 0) | (patch->lfo_rate & 7)));
    for (v = 0; v < GENISYS_NUM_VOICES; v++) write_voice_patch(e, v);
}

GenisysPsgSettings genisys_default_psg(void) {
    GenisysPsgSettings s;
    s.mode = GENISYS_PSG_OFF;
    s.level = 10;
    s.octave = 0;
    s.attack = 0;
    s.decay = 2;
    s.sustain = 12;
    s.release = 2;
    s.arp_speed = 2;
    s.noise_on = 0;
    s.noise_white = 1;
    s.noise_rate = 1;
    s.noise_volume = 10;
    return s;
}

void genisys_engine_set_psg(GenisysEngine *e, const GenisysPsgSettings *settings) {
    GenisysPsgSettings previous = e->psg_settings;
    e->psg_settings = *settings;
    if (e->psg_settings.level < 0) e->psg_settings.level = 0;
    if (e->psg_settings.level > 15) e->psg_settings.level = 15;
    engine_psg_apply_settings(e, &previous);
}

/* ---- Drums ---- */

static void write_dac(GenisysEngine *e, int value) {
    if (value != e->dac_last_written) {
        ym2612_chip_write(&e->chip, 0, 0x2A, (uint8_t)value);
        e->dac_last_written = value;
    }
}

void genisys_engine_set_drums(GenisysEngine *e, const GenisysDrumSettings *settings) {
    int was_enabled = e->drum_settings.enabled;
    e->drum_settings = *settings;
    if (e->drum_settings.level < 0) e->drum_settings.level = 0;
    if (e->drum_settings.level > 100) e->drum_settings.level = 100;

    if (settings->enabled && !was_enabled) {
        /* Channel 6 now belongs to the DAC: release any FM note it had. */
        GenisysVoice *v = &e->voice[GENISYS_NUM_VOICES - 1];
        if (v->state == GENISYS_VOICE_HELD) genisys_engine_note_off(e, v->note);
        v->state = GENISYS_VOICE_FREE;
        write_dac(e, 0x80);
        ym2612_chip_write(&e->chip, 0, 0x2B, 0x80);
    } else if (!settings->enabled && was_enabled) {
        e->dac_sample = NULL;
        write_dac(e, 0x80);
        ym2612_chip_write(&e->chip, 0, 0x2B, 0x00);
    }
}

void genisys_engine_drum_hit(GenisysEngine *e, int note, int velocity) {
    const GenisysDrum *drum = genisys_drum_for_note(note);
    float gain;

    if (!e->drum_settings.enabled || velocity <= 0) return;
    gain = engine_velocity_gain(velocity);

    if (drum->kind == GENISYS_DRUM_DAC) {
        /* One DAC channel: a new hit cuts off the previous one, as in games. */
        e->dac_sample = drum->sample;
        e->dac_length = drum->length;
        e->dac_pos = 0.0;
        e->dac_gain = gain * (float)e->drum_settings.level / 100.0f;
    } else if (drum->kind == GENISYS_DRUM_NOISE) {
        engine_psg_noise_drum(e, drum, velocity);
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

/* Channel 6 belongs to the DAC while drums are on. */
static int fm_voice_count(const GenisysEngine *e) {
    return e->drum_settings.enabled ? GENISYS_NUM_VOICES - 1 : GENISYS_NUM_VOICES;
}

static int pick_oldest(const GenisysEngine *e, GenisysVoiceState state, unsigned claimed) {
    int v, best = -1;
    for (v = 0; v < fm_voice_count(e); v++) {
        if (claimed & (1u << v)) continue;
        if (e->voice[v].state == state && (best < 0 || e->voice[v].age < e->voice[best].age)) best = v;
    }
    return best;
}

/* Preference order: retrigger the same note; else a silent voice; else the
 * voice released longest ago (its tail is the quietest); else steal the
 * oldest held note. Taking the *oldest* free voice rather than the lowest
 * numbered one means a release tail is never cut off while another voice
 * is available. */
/* `claimed` excludes voices already taken by this note-on (unison). */
static int allocate_voice(GenisysEngine *e, int note, unsigned claimed) {
    int v;

    for (v = 0; v < fm_voice_count(e); v++) {
        if (!(claimed & (1u << v)) && e->voice[v].state != GENISYS_VOICE_FREE && e->voice[v].note == note) return v;
    }
    for (v = 0; v < fm_voice_count(e); v++) {
        if (e->voice[v].state == GENISYS_VOICE_RELEASED && voice_is_silent(e, v)) {
            e->voice[v].state = GENISYS_VOICE_FREE;
        }
    }
    v = pick_oldest(e, GENISYS_VOICE_FREE, claimed);
    if (v < 0) v = pick_oldest(e, GENISYS_VOICE_RELEASED, claimed);
    if (v < 0) v = pick_oldest(e, GENISYS_VOICE_HELD, claimed);
    return v;
}

/* Unison layout for copy i of n: detune spread evenly around the note,
 * panned left / centre / right with the chip's hard pan switches. */
static double unison_detune(const GenisysEngine *e, int i, int n) {
    double cents = e->patch.unison_detune;
    if (n == 2) return (i == 0 ? -cents : cents) / 200.0;
    if (n == 3) return (i - 1) * cents / 100.0;
    return 0.0;
}

static uint8_t unison_pan(const GenisysEngine *e, int i, int n) {
    if (n < 2 || !e->patch.unison_stereo) return 0xC0;
    if (n == 2) return i == 0 ? 0x80 : 0x40;
    return i == 0 ? 0x80 : (i == 2 ? 0x40 : 0xC0);
}

static void start_voice(GenisysEngine *e, int v, int note, int velocity, double detune, uint8_t pan, int retrigger) {
    if (retrigger) key(e, v, 0); /* the chip only restarts the envelope on an off->on edge */

    e->voice[v].state = GENISYS_VOICE_HELD;
    e->voice[v].note = note;
    e->voice[v].velocity = velocity;
    e->voice[v].age = ++e->event_counter;
    e->voice[v].sustained = 0;
    e->voice[v].detune = detune;
    if (e->voice[v].pan != pan) {
        const GenisysPatch *p = &e->patch;
        e->voice[v].pan = pan;
        chip_write_channel(e, v, 0xB4, (uint8_t)(pan | ((p->ams & 3) << 4) | (p->pms & 7)));
    }

    e->voice[v].last_block = e->voice[v].last_fnum = -1; /* force the write */
    write_voice_pitch(e, v);
    write_voice_tl(e, v);
    if (retrigger) key(e, v, 1);
}

/* ---- Mono / Legato ---- */

static void stack_remove(GenisysEngine *e, int note) {
    int i, j = 0;
    for (i = 0; i < e->stack_count; i++) {
        if (e->note_stack[i] != note) e->note_stack[j++] = e->note_stack[i];
    }
    e->stack_count = j;
}

static void stack_push(GenisysEngine *e, int note) {
    stack_remove(e, note);
    if (e->stack_count == 16) stack_remove(e, e->note_stack[0]);
    e->note_stack[e->stack_count++] = note;
}

/* Moves the single mono note to `note`, gliding if Glide is set. */
static void mono_play(GenisysEngine *e, int note, int velocity, int retrigger) {
    int i, n = unison_count(e);
    double ticks = e->patch.glide_time / 1000.0 * (YM2612_SAMPLE_HZ / ENGINE_CONTROL_PERIOD);

    e->mono_target = note;
    if (e->patch.glide_time > 0 && e->mono_has_pitch && ticks >= 1.0) {
        /* Constant-time glide: any interval takes Glide ms. */
        e->glide_step = (e->mono_target - e->mono_pitch) / ticks;
    } else {
        e->mono_pitch = note;
        e->glide_step = 0.0;
    }
    e->mono_has_pitch = 1;
    e->mono_velocity = velocity;

    for (i = 0; i < n && i < fm_voice_count(e); i++) {
        start_voice(e, i, note, velocity, unison_detune(e, i, n), unison_pan(e, i, n), retrigger);
    }
}

static void mono_note_on(GenisysEngine *e, int note, int velocity) {
    int was_playing = e->stack_count > 0, previous = was_playing ? e->note_stack[e->stack_count - 1] : -1;
    stack_push(e, note);
    mono_play(e, note, velocity, !was_playing || e->patch.voice_mode == GENISYS_MODE_MONO);
    if (was_playing) engine_psg_note_off(e, previous);
    engine_psg_note_on(e, note);
}

static void mono_note_off(GenisysEngine *e, int note) {
    int was_top = e->stack_count > 0 && e->note_stack[e->stack_count - 1] == note, v;

    stack_remove(e, note);
    if (e->stack_count > 0) {
        if (was_top) {
            /* Fall back to the key still held (last-note priority). */
            int back = e->note_stack[e->stack_count - 1];
            mono_play(e, back, e->mono_velocity, e->patch.voice_mode == GENISYS_MODE_MONO);
            engine_psg_note_off(e, note);
            engine_psg_note_on(e, back);
        }
        return;
    }
    for (v = 0; v < unison_count(e) && v < fm_voice_count(e); v++) {
        if (e->voice[v].state != GENISYS_VOICE_HELD) continue;
        if (e->sustain_pedal) {
            e->voice[v].sustained = 1;
        } else {
            key(e, v, 0);
            e->voice[v].state = GENISYS_VOICE_RELEASED;
            e->voice[v].sustained = 0;
            e->voice[v].age = ++e->event_counter;
        }
    }
    if (!e->sustain_pedal) engine_psg_note_off(e, e->voice[0].note);
}

void genisys_engine_note_on(GenisysEngine *e, int note, int velocity) {
    int v;

    if (note < 0 || note > 127) return;
    if (velocity <= 0) { /* MIDI convention: note-on with velocity 0 is a note-off */
        genisys_engine_note_off(e, note);
        return;
    }
    if (velocity > 127) velocity = 127;

    if (is_mono(e)) {
        mono_note_on(e, note, velocity);
        return;
    }

    {
        int i, n = unison_count(e);
        unsigned claimed = 0;
        for (i = 0; i < n; i++) {
            v = allocate_voice(e, note, claimed);
            if (v < 0) break;
            claimed |= 1u << v;
            start_voice(e, v, note, velocity, unison_detune(e, i, n), unison_pan(e, i, n), 1);
        }
    }
    engine_psg_note_on(e, note);
}

void genisys_engine_note_off(GenisysEngine *e, int note) {
    int v;

    if (is_mono(e)) {
        mono_note_off(e, note);
        return;
    }

    if (e->sustain_pedal) {
        /* Keep sounding until the pedal comes up. */
        for (v = 0; v < GENISYS_NUM_VOICES; v++) {
            if (e->voice[v].state == GENISYS_VOICE_HELD && e->voice[v].note == note) e->voice[v].sustained = 1;
        }
        return;
    }
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD && e->voice[v].note == note) {
            e->voice[v].sustained = 0;
            key(e, v, 0);
            e->voice[v].state = GENISYS_VOICE_RELEASED;
            e->voice[v].age = ++e->event_counter;
        }
    }
    engine_psg_note_off(e, note);
}

void genisys_engine_pitch_bend(GenisysEngine *e, double semitones) {
    e->bend_semitones = semitones;
}

void genisys_engine_mod_wheel(GenisysEngine *e, double amount) {
    e->mod_wheel = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
}

void genisys_engine_sustain(GenisysEngine *e, int down) {
    int v;
    if (down) {
        e->sustain_pedal = 1;
        return;
    }
    e->sustain_pedal = 0;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD && e->voice[v].sustained) {
            genisys_engine_note_off(e, e->voice[v].note);
        }
    }
}

void genisys_engine_all_notes_off(GenisysEngine *e) {
    int v;
    e->sustain_pedal = 0;
    e->stack_count = 0; /* mono: nothing to fall back to */
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) genisys_engine_note_off(e, e->voice[v].note);
    }
    engine_psg_all_notes_off(e);
}

/* ---- Rendering ---- */

/* One native-rate (~53 kHz) sample of FM + PSG. The PSG ticks ~4.2 times
 * per FM sample; averaging those ticks is the area under its square waves
 * for that sample period, which is the right way to sample a signal that
 * only changes at tick boundaries. */
static void clock_native(GenisysEngine *e, float *left, float *right) {
    int32_t fm_l, fm_r, psg_sum = 0;
    int n, k;

    /* Control-rate pitch: bend and vibrato for every sounding voice. */
    if (++e->control_counter >= ENGINE_CONTROL_PERIOD) {
        int v;
        e->control_counter = 0;
        e->vibrato_phase += (e->patch.vibrato_rate / 10.0) * ENGINE_CONTROL_PERIOD / YM2612_SAMPLE_HZ;
        e->vibrato_phase -= floor(e->vibrato_phase);
        if (e->glide_step != 0.0) {
            e->mono_pitch += e->glide_step;
            if ((e->glide_step > 0.0) == (e->mono_pitch >= e->mono_target)) {
                e->mono_pitch = e->mono_target;
                e->glide_step = 0.0;
            }
        }
        for (v = 0; v < GENISYS_NUM_VOICES; v++) {
            if (e->voice[v].state != GENISYS_VOICE_FREE) write_voice_pitch(e, v);
        }
        engine_psg_update_pitch(e);
    }

    /* 60 Hz frame clock: PSG envelopes, arpeggio, noise drums. */
    e->frame_accum += 1.0;
    if (e->frame_accum >= ENGINE_SAMPLES_PER_FRAME) {
        e->frame_accum -= ENGINE_SAMPLES_PER_FRAME;
        engine_psg_frame(e);
    }

    /* DAC drum: feed the next 8-bit sample value, held between the sample's
     * own (lower) rate steps, exactly as a sound driver would. */
    if (e->dac_sample != NULL) {
        int i = (int)e->dac_pos;
        if (i >= e->dac_length) {
            e->dac_sample = NULL;
            write_dac(e, 0x80);
        } else {
            float v = (float)e->dac_sample[i] * e->dac_gain;
            int q = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            write_dac(e, q + 0x80);
            e->dac_pos += GENISYS_DRUM_SAMPLE_HZ / YM2612_SAMPLE_HZ;
        }
    }

    ym2612_chip_clock_wide(&e->chip, &fm_l, &fm_r);

    e->psg_tick_accum += e->psg_ticks_per_fm_sample;
    n = (int)e->psg_tick_accum;
    e->psg_tick_accum -= n;
    for (k = 0; k < n; k++) psg_sum += psg_clock(&e->psg);
    if (n > 0) psg_sum /= n;

    {
        float psg = (float)psg_sum * PSG_MIX_GAIN;
        float l = ((float)fm_l + psg) * OUTPUT_SCALE;
        float r = ((float)fm_r + psg) * OUTPUT_SCALE;

        /* DC blocker (one-pole high-pass, ~5 Hz). The console's output
         * capacitors do the same job: the ladder effect leaves a constant
         * offset even in silence, which would otherwise reach the DAW. Primed
         * with the first sample so a fresh instance starts silent. */
        if (!e->dc_primed) {
            e->dc_x_l = l; e->dc_x_r = r;
            e->dc_y_l = 0.0f; e->dc_y_r = 0.0f;
            e->dc_primed = 1;
        }
        e->dc_y_l = l - e->dc_x_l + e->dc_coeff * e->dc_y_l;
        e->dc_y_r = r - e->dc_x_r + e->dc_coeff * e->dc_y_r;
        e->dc_x_l = l;
        e->dc_x_r = r;
        l = e->dc_y_l;
        r = e->dc_y_r;

        /* Console low-pass: first order, like an RC filter. */
        if (e->console.filter_on) {
            e->lp_l = l + e->lp_coeff * (e->lp_l - l);
            e->lp_r = r + e->lp_coeff * (e->lp_r - r);
            l = e->lp_l;
            r = e->lp_r;
        } else {
            e->lp_l = l;
            e->lp_r = r;
        }

        /* Flush tiny values so long silences can't decay into denormals,
         * which are very slow on most CPUs. */
        if (fabsf(e->dc_y_l) < 1e-20f) e->dc_y_l = 0.0f;
        if (fabsf(e->dc_y_r) < 1e-20f) e->dc_y_r = 0.0f;
        if (fabsf(e->lp_l) < 1e-20f) e->lp_l = 0.0f;
        if (fabsf(e->lp_r) < 1e-20f) e->lp_r = 0.0f;

        *left = l;
        *right = r;
    }
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

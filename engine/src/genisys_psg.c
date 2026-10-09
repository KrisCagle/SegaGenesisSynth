/* The PSG layer: SN76489 tone channels under the FM voices (unison or
 * arpeggio), their frame-stepped envelopes, the noise drone, and the
 * noise-channel drums (hats and cymbals). */

#include "engine_internal.h"

#include <math.h>
#include <stddef.h>

/* ---- Register writes (the real SN76489 latch/data byte protocol) ---- */

static void psg_tone(GenisysEngine *e, int channel, double hz) {
    int n = (int)(PSG_CLOCK_HZ / (32.0 * hz) + 0.5);
    if (n > 0x3FF) n = 0x3FF;
    if (n < 1) n = 1;
    psg_write(&e->psg, (uint8_t)(0x80 | (channel << 5) | (n & 0x0F)));
    psg_write(&e->psg, (uint8_t)((n >> 4) & 0x3F));
}

static void psg_attenuation(GenisysEngine *e, int channel, int attenuation) {
    psg_write(&e->psg, (uint8_t)(0x80 | (channel << 5) | 0x10 | (attenuation & 0x0F)));
}

/* The PSG's lowest tone is ~109 Hz (10-bit divider). Notes below that are
 * moved up by octaves so the layer stays in harmony instead of going out of
 * tune or silent. Includes the current bend/vibrato. */
static double note_hz(const GenisysEngine *e, int note) {
    double hz = 440.0 * pow(2.0, (note + 12 * e->psg_settings.octave + engine_pitch_offset(e) - 69.0) / 12.0);
    while (hz < 110.0) hz *= 2.0;
    return hz;
}

/* ---- Tone voices ---- */

/* Both the envelope volume and the layer level are in the PSG's own
 * logarithmic 2 dB steps, so they combine by adding attenuations (dB add),
 * not by multiplying. */
static void write_voice_volume(GenisysEngine *e, int k) {
    int attenuation = (15 - e->psg_voice[k].volume) + (15 - e->psg_settings.level);
    psg_attenuation(e, k, attenuation > 15 ? 15 : attenuation);
}

static void enter_decay(GenisysEngine *e, GenisysPsgVoice *v) {
    const GenisysPsgSettings *s = &e->psg_settings;
    if (s->decay == 0 || v->volume <= s->sustain) {
        if (s->decay == 0) v->volume = s->sustain;
        v->stage = GENISYS_ENV_SUSTAIN;
    } else {
        v->stage = GENISYS_ENV_DECAY;
        v->counter = s->decay;
    }
}

static void enter_release(GenisysEngine *e, GenisysPsgVoice *v) {
    v->gate = 0;
    if (e->psg_settings.release == 0 || v->volume == 0) {
        v->volume = 0;
        v->stage = GENISYS_ENV_OFF;
    } else {
        v->stage = GENISYS_ENV_RELEASE;
        v->counter = e->psg_settings.release;
    }
}

static void start_voice(GenisysEngine *e, int k, int note) {
    GenisysPsgVoice *v = &e->psg_voice[k];
    v->note = note;
    v->gate = 1;
    v->age = e->event_counter;
    if (e->psg_settings.attack == 0) {
        v->volume = 15;
        enter_decay(e, v);
    } else {
        v->volume = 0;
        v->stage = GENISYS_ENV_ATTACK;
        v->counter = e->psg_settings.attack;
    }
    psg_tone(e, k, note_hz(e, note));
    write_voice_volume(e, k);
}

static void step_envelope(GenisysEngine *e, GenisysPsgVoice *v) {
    const GenisysPsgSettings *s = &e->psg_settings;
    switch (v->stage) {
        case GENISYS_ENV_ATTACK:
            if (--v->counter <= 0) {
                v->volume++;
                v->counter = s->attack;
                if (v->volume >= 15) {
                    v->volume = 15;
                    enter_decay(e, v);
                }
            }
            break;
        case GENISYS_ENV_DECAY:
            if (--v->counter <= 0) {
                v->volume--;
                v->counter = s->decay;
                if (v->volume <= s->sustain) {
                    v->volume = s->sustain;
                    v->stage = GENISYS_ENV_SUSTAIN;
                }
            }
            break;
        case GENISYS_ENV_RELEASE:
            if (--v->counter <= 0) {
                v->volume--;
                v->counter = s->release;
                if (v->volume <= 0) {
                    v->volume = 0;
                    v->stage = GENISYS_ENV_OFF;
                }
            }
            break;
        case GENISYS_ENV_SUSTAIN:
        case GENISYS_ENV_OFF:
        default:
            break;
    }
}

/* Held FM notes, lowest first: the arpeggio's pattern. */
static int held_notes(const GenisysEngine *e, int *notes) {
    int count = 0, v, i, j;
    for (v = 0; v < GENISYS_NUM_VOICES; v++) {
        if (e->voice[v].state == GENISYS_VOICE_HELD) notes[count++] = e->voice[v].note;
    }
    for (i = 1; i < count; i++) {
        int n = notes[i];
        for (j = i; j > 0 && notes[j - 1] > n; j--) notes[j] = notes[j - 1];
        notes[j] = n;
    }
    return count;
}

/* ---- Noise channel ---- */

static void apply_noise_drone(GenisysEngine *e) {
    const GenisysPsgSettings *s = &e->psg_settings;
    psg_write(&e->psg, (uint8_t)(0x80 | (3 << 5) | (s->noise_white ? 0x04 : 0) | (s->noise_rate & 3)));
    psg_attenuation(e, 3, s->noise_on ? 15 - s->noise_volume : 15);
}

static void write_noise_drum_volume(GenisysEngine *e) {
    /* Drum Level is a percentage of amplitude: convert it to PSG steps. */
    int level = e->drum_settings.level;
    float level_db = level > 0 ? (float)(-20.0 * log10(level / 100.0)) : 120.0f;
    int attenuation = (15 - e->noise_drum_volume_q / 4) + engine_db_to_psg_steps(level_db);
    psg_attenuation(e, 3, attenuation > 15 ? 15 : attenuation);
}

void engine_psg_noise_drum(GenisysEngine *e, const GenisysDrum *drum, int velocity) {
    int volume = drum->start_volume - engine_db_to_psg_steps(engine_velocity_db(velocity));
    if (volume < 0) volume = 0;
    /* Writing the noise control also resets the LFSR, so every hit starts
     * the same way -- the same as on hardware. */
    psg_write(&e->psg, (uint8_t)(0x80 | (3 << 5) | (drum->noise_white ? 0x04 : 0) | (drum->noise_rate & 3)));
    e->noise_drum_active = 1;
    e->noise_drum_volume_q = volume * 4;
    e->noise_drum_decay_q = drum->decay_per_frame;
    write_noise_drum_volume(e);
}

/* ---- Public-to-the-engine entry points ---- */

void engine_psg_reset(GenisysEngine *e) {
    int k;
    for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
        GenisysPsgVoice *v = &e->psg_voice[k];
        v->note = -1;
        v->gate = 0;
        v->stage = GENISYS_ENV_OFF;
        v->volume = 0;
        v->counter = 0;
        v->age = 0;
        psg_attenuation(e, k, 15);
    }
    e->arp_index = 0;
    e->arp_counter = 0;
    e->noise_drum_active = 0;
    apply_noise_drone(e);
}

void engine_psg_apply_settings(GenisysEngine *e, const GenisysPsgSettings *previous) {
    const GenisysPsgSettings *s = &e->psg_settings;
    int k;

    if (previous == NULL || previous->mode != s->mode || previous->octave != s->octave) {
        /* A different layer shape: start clean rather than leave stray notes. */
        engine_psg_reset(e);
        return;
    }
    /* Level changes take effect on notes already sounding. */
    for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
        if (e->psg_voice[k].stage != GENISYS_ENV_OFF) write_voice_volume(e, k);
    }
    if (!e->noise_drum_active) apply_noise_drone(e);
}

void engine_psg_note_on(GenisysEngine *e, int note) {
    int k, best = -1;

    if (e->psg_settings.mode == GENISYS_PSG_UNISON) {
        /* Same allocation rules as FM: retrigger the same note, else a
         * silent channel, else the longest-released, else the oldest. */
        for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
            if (e->psg_voice[k].stage != GENISYS_ENV_OFF && e->psg_voice[k].note == note) best = k;
        }
        for (k = 0; best < 0 && k < GENISYS_PSG_TONE_CHANNELS; k++) {
            if (e->psg_voice[k].stage == GENISYS_ENV_OFF) best = k;
        }
        for (k = 0; best < 0 && k < GENISYS_PSG_TONE_CHANNELS; k++) {
            if (!e->psg_voice[k].gate) best = k;
        }
        if (best < 0) {
            best = 0;
            for (k = 1; k < GENISYS_PSG_TONE_CHANNELS; k++) {
                if (e->psg_voice[k].age < e->psg_voice[best].age) best = k;
            }
        }
        start_voice(e, best, note);
    } else if (e->psg_settings.mode == GENISYS_PSG_ARPEGGIO) {
        /* The first key starts the arpeggio right away; later keys join the
         * pattern on its next step. */
        if (!e->psg_voice[0].gate) {
            start_voice(e, 0, note);
            e->arp_index = 0;
            e->arp_counter = e->psg_settings.arp_speed > 0 ? e->psg_settings.arp_speed : 1;
        }
    }
}

void engine_psg_note_off(GenisysEngine *e, int note) {
    int k;
    if (e->psg_settings.mode == GENISYS_PSG_UNISON) {
        for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
            GenisysPsgVoice *v = &e->psg_voice[k];
            if (v->gate && v->note == note) {
                enter_release(e, v);
                write_voice_volume(e, k);
            }
        }
    } else if (e->psg_settings.mode == GENISYS_PSG_ARPEGGIO) {
        int notes[GENISYS_NUM_VOICES];
        if (e->psg_voice[0].gate && held_notes(e, notes) == 0) {
            enter_release(e, &e->psg_voice[0]);
            write_voice_volume(e, 0);
        }
    }
}

void engine_psg_all_notes_off(GenisysEngine *e) {
    int k;
    for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
        if (e->psg_voice[k].gate) {
            enter_release(e, &e->psg_voice[k]);
            write_voice_volume(e, k);
        }
    }
}

void engine_psg_update_pitch(GenisysEngine *e) {
    int k;
    if (engine_pitch_offset(e) == 0.0 && e->psg_pitch_dirty == 0) return;
    for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
        if (e->psg_voice[k].stage != GENISYS_ENV_OFF && e->psg_voice[k].note >= 0) {
            psg_tone(e, k, note_hz(e, e->psg_voice[k].note));
        }
    }
    /* One more update after the offset returns to 0, to land back in tune. */
    e->psg_pitch_dirty = engine_pitch_offset(e) != 0.0;
}

void engine_psg_frame(GenisysEngine *e) {
    int k;

    if (e->psg_settings.mode == GENISYS_PSG_ARPEGGIO && e->psg_voice[0].gate) {
        int notes[GENISYS_NUM_VOICES];
        int count = held_notes(e, notes);
        if (count > 0 && --e->arp_counter <= 0) {
            e->arp_index = (e->arp_index + 1) % count;
            e->psg_voice[0].note = notes[e->arp_index];
            psg_tone(e, 0, note_hz(e, notes[e->arp_index]));
            e->arp_counter = e->psg_settings.arp_speed > 0 ? e->psg_settings.arp_speed : 1;
        }
    }

    for (k = 0; k < GENISYS_PSG_TONE_CHANNELS; k++) {
        GenisysPsgVoice *v = &e->psg_voice[k];
        if (v->stage == GENISYS_ENV_OFF) continue;
        step_envelope(e, v);
        write_voice_volume(e, k);
    }

    if (e->noise_drum_active) {
        e->noise_drum_volume_q -= e->noise_drum_decay_q;
        if (e->noise_drum_volume_q <= 0) {
            e->noise_drum_active = 0;
            apply_noise_drone(e);
        } else {
            write_noise_drum_volume(e);
        }
    }
}

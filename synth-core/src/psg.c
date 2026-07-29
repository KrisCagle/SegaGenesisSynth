#include "psg.h"

/* 2 dB-per-step attenuation table (index 0 = loudest, 15 = silence).
 * These are the values documented for the real SN76489 DAC (SMS Power PSG
 * docs): amplitude(i) = 828 * 10^(-2*i/20), rounded, with index 15 forced
 * to hard silence rather than a very quiet nonzero step. */
static const int16_t VOLUME_TABLE[16] = {
    828, 657, 522, 414, 329, 261, 207, 164,
    130, 104,  82,  65,  52,  41,  33,   0
};

/* Fixed noise-generator periods, in the same "internal tick" units as the
 * tone counters, selected by the 2-bit rate field (00/01/10). Rate 11
 * instead syncs the noise clock to tone channel 2's output edges. */
static const int16_t NOISE_DIVISOR[3] = { 0x10, 0x20, 0x40 };

/* White-noise feedback taps: bits 0 and 3 XORed together. This tap mask is
 * specific to the SN76489/A as used in Sega hardware (some other PSG
 * variants use a different mask). */
#define NOISE_WHITE_TAPS 0x0009

void psg_reset(Psg *psg) {
    int i;
    for (i = 0; i < 3; i++) {
        psg->tone_reg[i] = 0;
        psg->tone_counter[i] = 1;
        psg->tone_output[i] = 0;
    }
    psg->noise_control = 0;
    psg->noise_counter = NOISE_DIVISOR[0];
    psg->lfsr = 0x8000; /* single set bit, real chip's power-on LFSR state */

    for (i = 0; i < 4; i++) {
        psg->volume[i] = 0x0F; /* silent, matches real power-on state */
    }

    psg->latched_channel = 0;
    psg->latched_type = 0;
}

void psg_write(Psg *psg, uint8_t value) {
    if (value & 0x80) {
        /* LATCH/DATA byte: 1 cc t dddd */
        uint8_t channel = (value >> 5) & 0x03;
        uint8_t type    = (value >> 4) & 0x01;
        uint8_t data4   = value & 0x0F;

        psg->latched_channel = channel;
        psg->latched_type = type;

        if (channel == 3) {
            if (type == 0) {
                psg->noise_control = data4 & 0x07;
                psg->lfsr = 0x8000; /* writing noise control resets the LFSR */
                if ((psg->noise_control & 0x03) != 0x03) {
                    psg->noise_counter = NOISE_DIVISOR[psg->noise_control & 0x03];
                }
            } else {
                psg->volume[3] = data4;
            }
        } else {
            if (type == 0) {
                psg->tone_reg[channel] = (psg->tone_reg[channel] & 0x3F0) | data4;
            } else {
                psg->volume[channel] = data4;
            }
        }
    } else {
        /* DATA byte: 0 - dddddd, only meaningful as the second byte of a
         * tone frequency write (high 6 bits of the 10-bit register). */
        if (psg->latched_channel != 3 && psg->latched_type == 0) {
            uint8_t data6 = value & 0x3F;
            psg->tone_reg[psg->latched_channel] =
                (psg->tone_reg[psg->latched_channel] & 0x0F) | (uint16_t)(data6 << 4);
        }
    }
}

sample_t psg_clock(Psg *psg) {
    int i;
    int tone2_toggled = 0;
    int32_t mix = 0;

    /* --- Tone generators (channels 0-2): 10-bit down-counter, toggles its
     * output flip-flop and reloads on reaching zero. A reload value of 0 is
     * treated as 1, which is what the real counter hardware does (it can't
     * represent a true zero period) and naturally yields the highest
     * possible tone frequency rather than a divide-by-zero. */
    for (i = 0; i < 3; i++) {
        if (--psg->tone_counter[i] <= 0) {
            psg->tone_counter[i] = psg->tone_reg[i] == 0 ? 1 : (int16_t)psg->tone_reg[i];
            psg->tone_output[i] ^= 1;
            if (i == 2) {
                tone2_toggled = 1;
            }
        }
    }

    /* --- Noise generator --- */
    {
        uint8_t rate = psg->noise_control & 0x03;
        int shift = 0;

        if (rate == 0x03) {
            shift = tone2_toggled;
        } else if (--psg->noise_counter <= 0) {
            psg->noise_counter = NOISE_DIVISOR[rate];
            shift = 1;
        }

        if (shift) {
            uint16_t feedback;
            if (psg->noise_control & 0x04) {
                /* white noise: parity of the tapped bits */
                uint16_t tapped = psg->lfsr & NOISE_WHITE_TAPS;
                tapped ^= tapped >> 8;
                tapped ^= tapped >> 4;
                tapped ^= tapped >> 2;
                tapped ^= tapped >> 1;
                feedback = tapped & 1;
            } else {
                /* periodic noise: feed bit0 straight back, producing a
                 * regular tone-like pulse rather than pseudo-random noise */
                feedback = psg->lfsr & 1;
            }
            psg->lfsr = (uint16_t)((psg->lfsr >> 1) | (feedback << 15));
        }
    }

    /* --- Mix: each channel outputs a bipolar (+amplitude/-amplitude)
     * square wave so the channels sum around a zero DC level, matching the
     * AC-coupled output of the real chip. */
    for (i = 0; i < 3; i++) {
        int16_t amp = VOLUME_TABLE[psg->volume[i]];
        mix += psg->tone_output[i] ? amp : -amp;
    }
    {
        int16_t amp = VOLUME_TABLE[psg->volume[3]];
        mix += (psg->lfsr & 1) ? amp : -amp;
    }

    return (sample_t)mix;
}

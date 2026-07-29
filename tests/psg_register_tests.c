/* Sanity checks for psg.c against documented SN76489 behavior (SMS Power
 * PSG reference). Not exhaustive -- a first accuracy safety net that will
 * grow alongside the emulator. */

#include <stdio.h>
#include <math.h>
#include "psg.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static void set_tone(Psg *psg, int channel, uint16_t n) {
    psg_write(psg, (uint8_t)(0x80 | (channel << 5) | (n & 0x0F)));
    psg_write(psg, (uint8_t)((n >> 4) & 0x3F));
}

/* Two-byte 10-bit register write should reconstruct the exact N value. */
static void test_tone_register_reconstruction(void) {
    Psg psg;
    uint16_t values[] = { 0, 1, 0x0F, 0x0F0, 0x155, 0x3FF };
    size_t i;
    psg_reset(&psg);
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        set_tone(&psg, 0, values[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "tone_reg reconstructs N=0x%03X", values[i]);
        CHECK(psg.tone_reg[0] == values[i], msg);
    }
}

/* Output frequency = clock / (32 * N): count ticks between output
 * transitions and compare against the documented formula. */
static void test_tone_frequency(void) {
    Psg psg;
    uint16_t n = 100;
    int ticks = 0;
    uint8_t last;
    int transitions = 0;
    /* The counter carries over whatever value it had before the register
     * write (real hardware behavior -- a new N doesn't instantly resync
     * the counter), so the very first toggle after a cold reset can occur
     * early. Measure the period between the 2nd and 3rd toggle instead,
     * by which point the counter is reloading from the new N every time. */
    int tick_at_transition[3] = { -1, -1, -1 };

    psg_reset(&psg);
    set_tone(&psg, 0, n);
    psg_write(&psg, 0x80 | (0 << 5) | 0x10 | 0x00); /* channel 0 volume = loudest */

    last = psg.tone_output[0];
    while (transitions < 3) {
        psg_clock(&psg);
        ticks++;
        if (psg.tone_output[0] != last) {
            tick_at_transition[transitions] = ticks;
            last = psg.tone_output[0];
            transitions++;
        }
        if (ticks > 100000) break; /* safety valve */
    }

    CHECK(tick_at_transition[2] - tick_at_transition[1] == n,
          "tone toggles every N ticks (half-period == N)");
}

static void test_volume_table_monotonic_and_endpoints(void) {
    Psg psg;
    int i;
    int monotonic = 1;
    int16_t prev = 32767;

    psg_reset(&psg);
    /* Drive channel 0 at full frequency-independent DC-ish check isn't
     * possible directly (no accessor), so instead verify via psg_clock
     * amplitude while output is held high, sweeping attenuation. */
    set_tone(&psg, 0, 1); /* toggles every tick, so output alternates but
                              amplitude magnitude should still follow the
                              table -- check |sample| after volume write */
    for (i = 0; i <= 15; i++) {
        int16_t sample;
        psg_write(&psg, (uint8_t)(0x80 | (0 << 5) | 0x10 | i));
        sample = psg_clock(&psg);
        if (sample < 0) sample = (int16_t)-sample;
        if (sample > prev) monotonic = 0;
        prev = (int16_t)sample;
    }
    CHECK(monotonic, "attenuation table is non-increasing as index rises");

    psg_write(&psg, (uint8_t)(0x80 | (0 << 5) | 0x10 | 0x0F));
    CHECK(psg_clock(&psg) == 0, "attenuation index 15 is exact silence");
}

static void test_noise_control_resets_lfsr(void) {
    Psg psg;
    int i;
    psg_reset(&psg);
    psg_write(&psg, 0x80 | (0 << 5) | 0x10 | 0x00); /* loud tone ch0 to churn state */
    set_tone(&psg, 0, 3);
    for (i = 0; i < 50; i++) psg_clock(&psg); /* let noise clock/shift some */

    psg_write(&psg, 0x80 | (3 << 5) | 0x04); /* noise control: white, rate 0 */
    CHECK(psg.lfsr == 0x8000, "writing noise control resets LFSR to 0x8000");
}

int main(void) {
    test_tone_register_reconstruction();
    test_tone_frequency();
    test_volume_table_monotonic_and_endpoints();
    test_noise_control_resets_lfsr();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        printf("\n%d test(s) FAILED.\n", g_failures);
        return 1;
    }
}

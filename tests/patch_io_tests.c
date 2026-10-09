/* Tests for importing/exporting community YM2612 patch formats
 * (.tfi, .vgi, .dmp). Each test builds a file byte by byte from the
 * documented layout and checks every field lands on the right operator. */

#include <stdio.h>
#include <string.h>

#include "genisys_patch_io.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* Per file slot (register order OP1, OP3, OP2, OP4), distinct values so a
 * swapped operator would be caught. */
static const int MUL[4] = { 1, 3, 2, 4 };
static const int TL[4] = { 10, 30, 20, 40 };
static const int DT_FILE[4] = { 0, 6, 3, 4 };   /* -3, +3, none, +1 */
static const int DT_REG[4] = { 7, 3, 0, 1 };

static void fill_tfi_op(uint8_t *b, int slot) {
    b[0] = (uint8_t)MUL[slot];
    b[1] = (uint8_t)DT_FILE[slot];
    b[2] = (uint8_t)TL[slot];
    b[3] = (uint8_t)(slot & 3); /* KS */
    b[4] = (uint8_t)(31 - slot); /* AR */
    b[5] = (uint8_t)(10 + slot); /* DR */
    b[6] = (uint8_t)(5 + slot);  /* SR = D2R */
    b[7] = (uint8_t)(8 + slot);  /* RR */
    b[8] = (uint8_t)(2 + slot);  /* SL */
    b[9] = (uint8_t)(slot == 1 ? 0x0B : 0); /* SSG on, mode 3 for OP3 */
}

/* Logical op index (0=OP1..3=OP4) for a file slot. */
static int op_for_slot(int slot) { return slot == 1 ? 2 : (slot == 2 ? 1 : slot); }

static int ops_match(const GenisysPatch *p, int check_am) {
    int slot;
    for (slot = 0; slot < 4; slot++) {
        const GenisysOperatorParams *o = &p->op[op_for_slot(slot)];
        if (o->mul != MUL[slot] || o->tl != TL[slot] || o->dt != DT_REG[slot]) return 0;
        if (o->ks != (slot & 3) || o->ar != 31 - slot || o->d1r != 10 + slot) return 0;
        if (o->d2r != 5 + slot || o->rr != 8 + slot || o->sl != 2 + slot) return 0;
        if (o->ssg_enable != (slot == 1) || o->ssg_mode != (slot == 1 ? 3 : 0)) return 0;
        if (check_am && o->am != (slot == 3)) return 0;
    }
    return 1;
}

static void test_tfi(void) {
    uint8_t file[42];
    GenisysPatch p = genisys_default_patch();
    int slot;

    file[0] = 5; /* algorithm */
    file[1] = 6; /* feedback */
    for (slot = 0; slot < 4; slot++) fill_tfi_op(file + 2 + slot * 10, slot);

    p.voice_mode = GENISYS_MODE_LEGATO; /* a performance setting the import must keep */
    CHECK(genisys_patch_import(file, sizeof file, "Slap Bass.TFI", &p) == GENISYS_PATCH_OK, "TFI: imports");
    CHECK(p.algorithm == 5 && p.feedback == 6, "TFI: algorithm and feedback");
    CHECK(ops_match(&p, 0), "TFI: every operator field lands on the right operator (file order OP1 OP3 OP2 OP4)");
    CHECK(p.voice_mode == GENISYS_MODE_LEGATO, "TFI: performance settings outside the file are kept");
}

static void test_tfi_round_trip(void) {
    uint8_t file[42], again[42];
    GenisysPatch p = genisys_default_patch(), q = genisys_default_patch();
    int slot;

    file[0] = 2;
    file[1] = 4;
    for (slot = 0; slot < 4; slot++) fill_tfi_op(file + 2 + slot * 10, slot);
    genisys_patch_import(file, sizeof file, "a.tfi", &p);
    CHECK(genisys_patch_export_tfi(&p, again) == 42 && memcmp(file, again, 42) == 0,
          "TFI: export writes back exactly the bytes that were imported");
    genisys_patch_import(again, sizeof again, "b.tfi", &q);
    CHECK(memcmp(p.op, q.op, sizeof p.op) == 0, "TFI: export -> import gives the same patch");
}

static void test_vgi(void) {
    uint8_t file[43];
    GenisysPatch p = genisys_default_patch();
    int slot;

    file[0] = 4;
    file[1] = 2;
    file[2] = (uint8_t)((2 << 4) | 5); /* AMS 2, PMS 5 */
    for (slot = 0; slot < 4; slot++) {
        fill_tfi_op(file + 3 + slot * 10, slot);
        if (slot == 3) file[3 + slot * 10 + 5] |= 0x80; /* AM on OP4, in DR's top bit */
    }
    CHECK(genisys_patch_import(file, sizeof file, "brass.vgi", &p) == GENISYS_PATCH_OK, "VGI: imports");
    CHECK(p.algorithm == 4 && p.feedback == 2 && p.ams == 2 && p.pms == 5 && p.lfo_enable,
          "VGI: algorithm, feedback, LFO sensitivities (and turns the LFO on)");
    CHECK(ops_match(&p, 1), "VGI: operators, including the AM bit packed into DR");
}

static void test_dmp(void) {
    uint8_t file[7 + 44];
    GenisysPatch p = genisys_default_patch();
    int slot, pos = 0;

    file[pos++] = 11;  /* version */
    file[pos++] = 2;   /* system: Genesis */
    file[pos++] = 1;   /* mode: FM */
    file[pos++] = 3;   /* FMS (PMS) */
    file[pos++] = 7;   /* FB */
    file[pos++] = 6;   /* ALG */
    file[pos++] = 1;   /* AMS */
    for (slot = 0; slot < 4; slot++) {
        uint8_t *b = file + pos + slot * 11;
        b[0] = (uint8_t)MUL[slot];
        b[1] = (uint8_t)TL[slot];
        b[2] = (uint8_t)(31 - slot);
        b[3] = (uint8_t)(10 + slot);
        b[4] = (uint8_t)(2 + slot);
        b[5] = (uint8_t)(8 + slot);
        b[6] = (uint8_t)(slot == 3);
        b[7] = (uint8_t)(slot & 3);
        b[8] = (uint8_t)(DT_FILE[slot] | 0x20); /* high nibble: OPM-only DT2, must be ignored */
        b[9] = (uint8_t)(5 + slot);
        b[10] = (uint8_t)(slot == 1 ? 0x0B : 0);
    }
    CHECK(genisys_patch_import(file, sizeof file, "lead.dmp", &p) == GENISYS_PATCH_OK, "DMP v11: imports");
    CHECK(p.algorithm == 6 && p.feedback == 7 && p.pms == 3 && p.ams == 1, "DMP v11: algorithm, feedback, LFO sensitivities");
    CHECK(ops_match(&p, 1), "DMP v11: operators (ignoring the OPM-only DT2 nibble)");

    file[1] = 4; /* a Game Boy preset */
    CHECK(genisys_patch_import(file, sizeof file, "gb.dmp", &p) == GENISYS_PATCH_NOT_FM, "DMP: other systems are rejected");
}

static void test_bad_files(void) {
    uint8_t small[10] = { 0 };
    GenisysPatch p = genisys_default_patch(), before = p;
    CHECK(genisys_patch_import(small, sizeof small, "short.tfi", &p) == GENISYS_PATCH_TRUNCATED,
          "a too-short file is reported as truncated");
    CHECK(genisys_patch_import(small, sizeof small, "song.wav", &p) == GENISYS_PATCH_UNKNOWN_FORMAT,
          "an unrelated file type is rejected");
    CHECK(memcmp(&p, &before, sizeof p) == 0, "a failed import leaves the patch untouched");
}

int main(void) {
    test_tfi();
    test_tfi_round_trip();
    test_vgi();
    test_dmp();
    test_bad_files();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

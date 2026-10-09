#include "genisys_patch_io.h"

#include <ctype.h>
#include <string.h>

/* File operator slot -> logical operator (files use register order). */
static const int FILE_TO_OP[4] = { 0, 2, 1, 3 };

/* File detune (0-6, 3 = none; 7 also none) -> YM2612 DT register field. */
static const int FILE_TO_DT[8] = { 7, 6, 5, 0, 1, 2, 3, 4 };

/* DT register field -> file detune. */
static const int DT_TO_FILE[8] = { 3, 4, 5, 6, 3, 2, 1, 0 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int has_extension(const char *filename, const char *ext) {
    size_t n, m = strlen(ext), i;
    if (filename == NULL) return 0;
    n = strlen(filename);
    if (n < m) return 0;
    for (i = 0; i < m; i++) {
        if (tolower((unsigned char)filename[n - m + i]) != ext[i]) return 0;
    }
    return 1;
}

/* One operator's fields, in the units every format shares. */
typedef struct {
    int mul, dt_file, tl, ks, ar, d1r, d2r, rr, sl, ssg, am;
} FileOp;

static void store_op(GenisysPatch *p, int file_slot, const FileOp *f) {
    GenisysOperatorParams *o = &p->op[FILE_TO_OP[file_slot]];
    o->mul = clampi(f->mul, 0, 15);
    o->dt = FILE_TO_DT[f->dt_file & 7];
    o->tl = clampi(f->tl, 0, 127);
    o->ks = clampi(f->ks, 0, 3);
    o->ar = clampi(f->ar, 0, 31);
    o->d1r = clampi(f->d1r, 0, 31);
    o->d2r = clampi(f->d2r, 0, 31);
    o->rr = clampi(f->rr, 0, 15);
    o->sl = clampi(f->sl, 0, 15);
    o->ssg_enable = (f->ssg & 0x08) ? 1 : 0;
    o->ssg_mode = f->ssg & 0x07;
    o->am = f->am ? 1 : 0;
}

/* TFI and VGI share the per-operator layout:
 * MUL DT TL RS(KS) AR DR SR(D2R) RR SL SSG. VGI keeps AM in DR's top bit. */
static void read_tfi_vgi_op(const uint8_t *b, FileOp *f, int vgi) {
    f->mul = b[0];
    f->dt_file = b[1];
    f->tl = b[2];
    f->ks = b[3];
    f->ar = b[4];
    f->d1r = vgi ? (b[5] & 0x7F) : b[5];
    f->am = vgi ? ((b[5] & 0x80) != 0) : 0;
    f->d2r = b[6];
    f->rr = b[7];
    f->sl = b[8];
    f->ssg = b[9];
}

static GenisysPatchResult import_tfi(const uint8_t *d, size_t size, GenisysPatch *p) {
    GenisysPatch out = *p;
    int i;
    if (size < GENISYS_TFI_SIZE) return GENISYS_PATCH_TRUNCATED;
    out.algorithm = d[0] & 7;
    out.feedback = d[1] & 7;
    for (i = 0; i < 4; i++) {
        FileOp f;
        read_tfi_vgi_op(d + 2 + i * 10, &f, 0);
        store_op(&out, i, &f);
    }
    *p = out;
    return GENISYS_PATCH_OK;
}

static GenisysPatchResult import_vgi(const uint8_t *d, size_t size, GenisysPatch *p) {
    GenisysPatch out = *p;
    int i;
    if (size < 43) return GENISYS_PATCH_TRUNCATED;
    out.algorithm = d[0] & 7;
    out.feedback = d[1] & 7;
    out.pms = d[2] & 7;
    out.ams = (d[2] >> 4) & 3;
    for (i = 0; i < 4; i++) {
        FileOp f;
        read_tfi_vgi_op(d + 3 + i * 10, &f, 1);
        store_op(&out, i, &f);
    }
    if (out.pms > 0 || out.ams > 0) out.lfo_enable = 1;
    *p = out;
    return GENISYS_PATCH_OK;
}

/* DefleMask preset. Version 11 (DefleMask 1.0+) adds a system byte; older
 * versions store whether the instrument is FM, and version 9 an operator
 * count. FM operators: MUL TL AR DR SL RR AM RS DT D2R SSG. */
static GenisysPatchResult import_dmp(const uint8_t *d, size_t size, GenisysPatch *p) {
    GenisysPatch out = *p;
    size_t pos = 0;
    int version, i;

    if (size < 1) return GENISYS_PATCH_TRUNCATED;
    version = d[pos++];
    if (version < 9 || version > 11) return GENISYS_PATCH_UNKNOWN_FORMAT;

    if (version == 11) {
        if (size < 2) return GENISYS_PATCH_TRUNCATED;
        if (d[pos++] != 2) return GENISYS_PATCH_NOT_FM; /* 2 = Genesis */
    }
    if (pos >= size) return GENISYS_PATCH_TRUNCATED;
    if (d[pos++] != 1) return GENISYS_PATCH_NOT_FM;  /* instrument mode: 1 = FM */
    if (version == 9) pos++;                          /* operator count byte */

    if (pos + 4 + 4 * 11 > size) return GENISYS_PATCH_TRUNCATED;
    out.pms = d[pos++] & 7;
    out.feedback = d[pos++] & 7;
    out.algorithm = d[pos++] & 7;
    out.ams = d[pos++] & 3;

    for (i = 0; i < 4; i++) {
        const uint8_t *b = d + pos + i * 11;
        FileOp f;
        f.mul = b[0];
        f.tl = b[1];
        f.ar = b[2];
        f.d1r = b[3];
        f.sl = b[4];
        f.rr = b[5];
        f.am = b[6];
        f.ks = b[7];
        f.dt_file = b[8] & 0x0F; /* the high nibble is DT2, an OPM-only field */
        f.d2r = b[9];
        f.ssg = b[10];
        store_op(&out, i, &f);
    }
    if (out.pms > 0 || out.ams > 0) out.lfo_enable = 1;
    *p = out;
    return GENISYS_PATCH_OK;
}

GenisysPatchResult genisys_patch_import(const uint8_t *data, size_t size, const char *filename, GenisysPatch *patch) {
    if (has_extension(filename, ".tfi")) return import_tfi(data, size, patch);
    if (has_extension(filename, ".vgi")) return import_vgi(data, size, patch);
    if (has_extension(filename, ".dmp")) return import_dmp(data, size, patch);
    return GENISYS_PATCH_UNKNOWN_FORMAT;
}

size_t genisys_patch_export_tfi(const GenisysPatch *p, uint8_t out[GENISYS_TFI_SIZE]) {
    int i;
    out[0] = (uint8_t)(p->algorithm & 7);
    out[1] = (uint8_t)(p->feedback & 7);
    for (i = 0; i < 4; i++) {
        const GenisysOperatorParams *o = &p->op[FILE_TO_OP[i]];
        uint8_t *b = out + 2 + i * 10;
        b[0] = (uint8_t)(o->mul & 15);
        b[1] = (uint8_t)DT_TO_FILE[o->dt & 7];
        b[2] = (uint8_t)(o->tl & 127);
        b[3] = (uint8_t)(o->ks & 3);
        b[4] = (uint8_t)(o->ar & 31);
        b[5] = (uint8_t)(o->d1r & 31);
        b[6] = (uint8_t)(o->d2r & 31);
        b[7] = (uint8_t)(o->rr & 15);
        b[8] = (uint8_t)(o->sl & 15);
        b[9] = (uint8_t)((o->ssg_enable ? 0x08 : 0) | (o->ssg_mode & 7));
    }
    return GENISYS_TFI_SIZE;
}

const char *genisys_patch_result_text(GenisysPatchResult result) {
    switch (result) {
        case GENISYS_PATCH_OK: return "OK";
        case GENISYS_PATCH_UNKNOWN_FORMAT: return "Not a supported patch file (.tfi, .vgi or .dmp).";
        case GENISYS_PATCH_TRUNCATED: return "The file is shorter than its format requires.";
        case GENISYS_PATCH_NOT_FM: return "This DefleMask preset isn't a Genesis FM instrument.";
        default: return "Unknown error.";
    }
}

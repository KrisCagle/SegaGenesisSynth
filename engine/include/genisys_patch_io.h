#ifndef GENISYS_PATCH_IO_H
#define GENISYS_PATCH_IO_H

/* Reading and writing the community's YM2612 instrument formats, so the
 * thousands of existing Genesis patches can be loaded into Genisys:
 *
 *   .tfi  TFM Music Maker (42 bytes)
 *   .vgi  VGM Music Maker (43 bytes: TFI + LFO sensitivity + AM bits)
 *   .dmp  DefleMask preset (FM instruments, file versions 9-11)
 *
 * Layouts as read by the Furnace tracker's importers. All three list the
 * operators in the chip's register order (OP1, OP3, OP2, OP4) and store
 * detune as 0-6 with 3 meaning "no detune". */

#include <stddef.h>
#include <stdint.h>

#include "genisys_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GENISYS_PATCH_OK = 0,
    GENISYS_PATCH_UNKNOWN_FORMAT, /* not a .tfi / .vgi / .dmp */
    GENISYS_PATCH_TRUNCATED,      /* file too short for its format */
    GENISYS_PATCH_NOT_FM          /* a DefleMask preset for another chip / a non-FM instrument */
} GenisysPatchResult;

/* Decodes `data` into `patch`. The format comes from `filename`'s
 * extension. Only the sound-defining fields change: whatever `patch`
 * already held for performance settings (voice mode, vibrato, velocity...)
 * is kept, so pass in the current patch. On error `patch` is untouched. */
GenisysPatchResult genisys_patch_import(const uint8_t *data, size_t size, const char *filename, GenisysPatch *patch);

#define GENISYS_TFI_SIZE 42

/* Encodes the FM part of `patch` as a TFI file. Returns GENISYS_TFI_SIZE. */
size_t genisys_patch_export_tfi(const GenisysPatch *patch, uint8_t out[GENISYS_TFI_SIZE]);

const char *genisys_patch_result_text(GenisysPatchResult result);

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_PATCH_IO_H */

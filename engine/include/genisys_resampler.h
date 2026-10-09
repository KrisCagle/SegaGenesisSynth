#ifndef GENISYS_RESAMPLER_H
#define GENISYS_RESAMPLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Band-limited stereo resampler: converts the chip's native ~53,267 Hz
 * stream to whatever rate the host runs at (44.1k, 48k, 96k, ...).
 *
 * Why not just average or pick the nearest chip sample? Averaging 1 or 2
 * samples depending on rounding gives every output sample a slightly
 * different filter, which adds grit the real console doesn't have; and at
 * host rates *above* the chip rate the old code ran the chip once per output
 * sample, so everything played sharp. A windowed-sinc filter evaluated at
 * the exact fractional position fixes both: same filter for every sample,
 * correct pitch at any rate, and content above the output's Nyquist
 * frequency is removed instead of folding back as aliasing.
 *
 * Implementation: a Kaiser-windowed sinc, precomputed for RS_PHASES
 * fractional offsets and linearly interpolated between them. When
 * downsampling, the cutoff drops to just under the output Nyquist frequency
 * and the kernel widens to keep the same transition sharpness. */

#define RS_BASE_HALF 16  /* taps per side when not downsampling */
#define RS_MAX_HALF 48   /* cap, reached for output rates below ~18 kHz */
#define RS_PHASES 256
#define RS_BUF_LEN 2048  /* input history, in chip samples */

typedef struct {
    double step;   /* input samples advanced per output sample */
    double pos;    /* position of the next output sample in buf, in input samples */
    int half;      /* taps per side for the current ratio */
    int buf_len;   /* number of valid samples in buf_l/buf_r */
    float buf_l[RS_BUF_LEN];
    float buf_r[RS_BUF_LEN];
    float table[(RS_PHASES + 1) * 2 * RS_MAX_HALF];
} GenisysResampler;

void genisys_resampler_init(GenisysResampler *rs, double in_rate, double out_rate);

/* Number of input samples that must be appended before the next output
 * sample can be read. */
int genisys_resampler_needed(const GenisysResampler *rs);

void genisys_resampler_push(GenisysResampler *rs, float left, float right);

/* Call only when genisys_resampler_needed() returns 0. */
void genisys_resampler_read(GenisysResampler *rs, float *left, float *right);

#ifdef __cplusplus
}
#endif

#endif /* GENISYS_RESAMPLER_H */

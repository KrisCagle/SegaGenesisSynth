#include "genisys_resampler.h"

#include <math.h>
#include <string.h>

#define RS_KAISER_BETA 8.0 /* ~80 dB stopband: well below audibility */
#define RS_PASSBAND 0.92   /* cutoff as a fraction of the lower Nyquist */
#define RS_PI 3.14159265358979323846

/* Zeroth-order modified Bessel function, for the Kaiser window. */
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, half_x = x * 0.5;
    int k;
    for (k = 1; k < 50; k++) {
        term *= (half_x / k) * (half_x / k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

void genisys_resampler_init(GenisysResampler *rs, double in_rate, double out_rate) {
    double scale = out_rate < in_rate ? out_rate / in_rate : 1.0;
    double fc = 0.5 * scale * RS_PASSBAND; /* cutoff, in cycles per input sample */
    double i0_beta = bessel_i0(RS_KAISER_BETA);
    int half = (int)ceil(RS_BASE_HALF / scale);
    int taps, p, j;

    if (half > RS_MAX_HALF) half = RS_MAX_HALF;
    taps = 2 * half;

    rs->step = in_rate / out_rate;
    rs->half = half;

    for (p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES;
        float *row = &rs->table[p * taps];
        double sum = 0.0;

        for (j = 0; j < taps; j++) {
            double t = (double)(j - half + 1) - frac; /* distance from the output point */
            double x = t / half;
            double sinc = (fabs(t) < 1e-9) ? 1.0 : sin(2.0 * RS_PI * fc * t) / (2.0 * RS_PI * fc * t);
            double window = (fabs(x) <= 1.0) ? bessel_i0(RS_KAISER_BETA * sqrt(1.0 - x * x)) / i0_beta : 0.0;
            double h = 2.0 * fc * sinc * window;
            row[j] = (float)h;
            sum += h;
        }
        /* Normalize each phase to unity gain at DC, so a steady signal never
         * wobbles in level as the fractional position moves. */
        for (j = 0; j < taps; j++) row[j] = (float)(row[j] / sum);
    }

    /* Prime with silence so the first output sample has a full window of
     * history; this is the resampler's (sub-millisecond) latency. */
    memset(rs->buf_l, 0, sizeof rs->buf_l);
    memset(rs->buf_r, 0, sizeof rs->buf_r);
    rs->buf_len = half;
    rs->pos = half - 1;
}

int genisys_resampler_needed(const GenisysResampler *rs) {
    int n = (int)rs->pos;
    int need = n + rs->half + 1 - rs->buf_len;
    return need > 0 ? need : 0;
}

void genisys_resampler_push(GenisysResampler *rs, float left, float right) {
    if (rs->buf_len == RS_BUF_LEN) {
        /* Drop history the filter window has already passed. */
        int start = (int)rs->pos - rs->half + 1;
        int keep = rs->buf_len - start;
        memmove(rs->buf_l, rs->buf_l + start, (size_t)keep * sizeof(float));
        memmove(rs->buf_r, rs->buf_r + start, (size_t)keep * sizeof(float));
        rs->buf_len = keep;
        rs->pos -= start;
    }
    rs->buf_l[rs->buf_len] = left;
    rs->buf_r[rs->buf_len] = right;
    rs->buf_len++;
}

void genisys_resampler_read(GenisysResampler *rs, float *left, float *right) {
    int n = (int)rs->pos;
    double frac_phase = (rs->pos - n) * RS_PHASES;
    int p = (int)frac_phase;
    float f = (float)(frac_phase - p);
    int taps = 2 * rs->half;
    const float *row0 = &rs->table[p * taps];
    const float *row1 = row0 + taps;
    const float *in_l = &rs->buf_l[n - rs->half + 1];
    const float *in_r = &rs->buf_r[n - rs->half + 1];
    float acc_l = 0.0f, acc_r = 0.0f;
    int j;

    for (j = 0; j < taps; j++) {
        float c = row0[j] + f * (row1[j] - row0[j]);
        acc_l += in_l[j] * c;
        acc_r += in_r[j] * c;
    }

    *left = acc_l;
    *right = acc_r;
    rs->pos += rs->step;
}

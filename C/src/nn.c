/*
 * src/nn.c - Neutral Network
 */

#include "nn.h"

#include "nl_math.h"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

typedef float v4 __attribute__((vector_size(16), aligned(4)));

float nn_dot(const float *a, const float *b, size_t n) {
  v4 s0 = {0, 0, 0, 0};
  v4 s1 = {0, 0, 0, 0};
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    v4 a0 = *(const v4 *)(a + i);
    v4 b0 = *(const v4 *)(b + i);
    v4 a1 = *(const v4 *)(a + i + 4);
    v4 b1 = *(const v4 *)(b + i + 4);
    s0 += a0 * b0;
    s1 += a1 * b1;
  }

  s0 += s1;
  float r = s0[0] + s0[1] + s0[2] + s0[3];
  for (; i < n; i++) {
    r += a[i] * b[i];
  }

  return r;
}

// NOTE: 4 output units share each load of the input row, so the input is read
// once per four dot products instead of once per product
static void nn_linear_sse(float *y, const float *x, size_t n, const float *w,
                          const float *b, size_t in, size_t out) {
  for (size_t r = 0; r < n; r++) {
    const float *xr = x + r * in;
    float *yr = y + r * out;
    size_t o = 0;
    for (; o + 4 <= out; o += 4) {
      const float *w0 = w + o * in;
      const float *w1 = w0 + in;
      const float *w2 = w1 + in;
      const float *w3 = w2 + in;
      v4 s0 = {0, 0, 0, 0}, s1 = s0, s2 = s0, s3 = s0;
      size_t i = 0;
      for (; i + 4 <= in; i += 4) {
        v4 xv = *(const v4 *)(xr + i);
        s0 += xv * *(const v4 *)(w0 + i);
        s1 += xv * *(const v4 *)(w1 + i);
        s2 += xv * *(const v4 *)(w2 + i);
        s3 += xv * *(const v4 *)(w3 + i);
      }

      float a0 = s0[0] + s0[1] + s0[2] + s0[3];
      float a1 = s1[0] + s1[1] + s1[2] + s1[3];
      float a2 = s2[0] + s2[1] + s2[2] + s2[3];
      float a3 = s3[0] + s3[1] + s3[2] + s3[3];
      for (; i < in; i++) {
        a0 += xr[i] * w0[i];
        a1 += xr[i] * w1[i];
        a2 += xr[i] * w2[i];
        a3 += xr[i] * w3[i];
      }

      yr[o] = a0 + (b ? b[o] : 0.0f);
      yr[o + 1] = a1 + (b ? b[o + 1] : 0.0f);
      yr[o + 2] = a2 + (b ? b[o + 2] : 0.0f);
      yr[o + 3] = a3 + (b ? b[o + 3] : 0.0f);
    }

    for (; o < out; o++) {
      yr[o] = nn_dot(xr, w + o * in, in) + (b ? b[o] : 0.0f);
    }
  }
}

// NOTE: 8-wide AVX2 twin of nn_linear_sse: 2 output units per pass, each doing
// one 8-float fused multiply-add per 8 inputs instead of four 4-float
// multiplies and an add Only called once nn_has_avx2() has approved
#if defined(__x86_64__) || defined(__i386__)
typedef float v8 __attribute__((vector_size(32), aligned(4)));

__attribute__((target("avx2,fma"))) static void
nn_linear_avx2(float *y, const float *x, size_t n, const float *w,
               const float *b, size_t in, size_t out) {
  for (size_t r = 0; r < n; r++) {
    const float *xr = x + r * in;
    float *yr = y + r * out;
    size_t o = 0;
    for (; o + 2 <= out; o += 2) {
      const float *w0 = w + o * in;
      const float *w1 = w0 + in;
      v8 s0 = {0}, s1 = {0};
      size_t i = 0;
      for (; i + 8 <= in; i += 8) {
        v8 xv = *(const v8 *)(xr + i);
        s0 += xv * *(const v8 *)(w0 + i);
        s1 += xv * *(const v8 *)(w1 + i);
      }

      float a0 = s0[0] + s0[1] + s0[2] + s0[3] + s0[4] + s0[5] + s0[6] + s0[7];
      float a1 = s1[0] + s1[1] + s1[2] + s1[3] + s1[4] + s1[5] + s1[6] + s1[7];
      for (; i < in; i++) {
        a0 += xr[i] * w0[i];
        a1 += xr[i] * w1[i];
      }

      yr[o] = a0 + (b ? b[o] : 0.0f);
      yr[o + 1] = a1 + (b ? b[o + 1] : 0.0f);
    }

    for (; o < out; o++) {
      yr[o] = nn_dot(xr, w + o * in, in) + (b ? b[o] : 0.0f);
    }
  }
}

// NOTE: cpuid leaf 7, ebx bit 5 is AVX2; OS support (XGETBV) has to be checked
// too, since a CPU can support AVX2 while kernel has not enabled extended state
// that holds its registers
static int nn_has_avx2(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }

  cached = 0;
  if (__get_cpuid_max(0, 0) >= 7) {
    unsigned eax1;
    unsigned ebx1;
    unsigned ecx1;
    unsigned edx1;
    __cpuid_count(1, 0, eax1, ebx1, ecx1, edx1);
    int osxsave = (int)((ecx1 >> 27u) & 1u);
    if (osxsave) {
      unsigned xcr0_lo, xcr0_hi;
      __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
      int os_avx = (int)(xcr0_lo & 0x6u) == 0x6; // XMM and YMM state both saved

      if (os_avx) {
        unsigned eax7;
        unsigned ebx7;
        unsigned ecx7;
        unsigned edx7;
        __cpuid_count(7, 0, eax7, ebx7, ecx7, edx7);
        cached = (int)((ebx7 >> 5u) & 1u);
      }
    }
  }

  return cached;
}
#endif

void nn_linear(float *y, const float *x, size_t n, const float *w,
               const float *b, size_t in, size_t out) {
#if defined(__x86_64__) || defined(__i386__)
  if (nn_has_avx2()) {
    nn_linear_avx2(y, x, n, w, b, in, out);

    return;
  }
#endif

  nn_linear_sse(y, x, n, w, b, in, out);
}

void nn_layernorm(float *y, const float *x, size_t n, size_t dim, float eps,
                  const float *g, const float *b) {
  for (size_t r = 0; r < n; r++) {
    const float *xr = x + r * dim;
    float *yr = y + r * dim;
    double mean = 0;
    for (size_t i = 0; i < dim; i++) {
      mean += xr[i];
    }

    mean /= (double)dim;
    double var = 0;
    for (size_t i = 0; i < dim; i++) {
      double d = xr[i] - mean;
      var += d * d;
    }

    var /= (double)dim;
    float inv = (float)(1.0 / nl_sqrt(var + (double)eps));
    for (size_t i = 0; i < dim; i++) {
      float v = (float)(xr[i] - mean) * inv;
      if (g) {
        v *= g[i];
      }

      if (b) {
        v += b[i];
      }

      yr[i] = v;
    }
  }
}

void nn_gelu(float *x, size_t n) {
  for (size_t i = 0; i < n; i++) {
    x[i] = (float)(0.5 * x[i] * (1.0 + nl_erf(x[i] * 0.70710678118654752)));
  }
}

void nn_silu(float *x, size_t n) {
  for (size_t i = 0; i < n; i++) {
    x[i] = (float)(x[i] / (1.0 + nl_exp(-(double)x[i])));
  }
}

void nn_softmax(float *x, size_t n) {
  if (n == 0) {
    return;
  }

  float m = x[0];
  for (size_t i = 1; i < n; i++) {
    if (x[i] > m) {
      m = x[i];
    }
  }

  double sum = 0;
  for (size_t i = 0; i < n; i++) {
    double e = nl_exp((double)x[i] - (double)m);
    x[i] = (float)e;
    sum += e;
  }

  float inv = (float)(1.0 / sum);
  for (size_t i = 0; i < n; i++) {
    x[i] *= inv;
  }
}

void nn_attend(float *out, const float *q, const float *k, const float *v,
               size_t stride, size_t ring, size_t first, size_t nk,
               size_t heads, size_t hd, float *scratch) {
  float scale = (float)(1.0 / nl_sqrt((double)hd));
  for (size_t h = 0; h < heads; h++) {
    const float *qh = q + h * hd;
    for (size_t r = 0; r < nk; r++) {
      size_t row = first + r;
      if (ring) {
        row %= ring;
      }

      scratch[r] = nn_dot(qh, k + row * stride + h * hd, hd) * scale;
    }

    nn_softmax(scratch, nk);
    float *oh = out + h * hd;
    for (size_t d = 0; d < hd; d++) {
      oh[d] = 0;
    }

    for (size_t r = 0; r < nk; r++) {
      size_t row = first + r;
      if (ring) {
        row %= ring;
      }

      const float *vr = v + row * stride + h * hd;
      float w = scratch[r];
      for (size_t d = 0; d < hd; d++) {
        oh[d] += w * vr[d];
      }
    }
  }
}

void nn_rope(float *x, size_t heads, size_t hd, size_t rot, size_t pos,
             float theta) {
  for (size_t i = 0; i < rot / 2; i++) {
    double inv =
        nl_exp(-nl_log((double)theta) * (2.0 * (double)i) / (double)rot);
    double a = (double)pos * inv;
    float c = (float)nl_cos(a);
    float s = (float)nl_sin(a);
    for (size_t h = 0; h < heads; h++) {
      float *p = x + h * hd + 2 * i;
      float x1 = p[0], x2 = p[1];
      p[0] = x1 * c - x2 * s;
      p[1] = x2 * c + x1 * s;
    }
  }
}

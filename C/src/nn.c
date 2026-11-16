/*
 * src/nn.c - Neutral Network
 */

#include "nn.h"

#include "nl_math.h"

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

void nn_linear(float *y, const float *x, size_t n, const float *w,
               const float *b, size_t in, size_t out) {
  for (size_t r = 0; r < n; r++) {
    const float *xr = x + r * in;
    float *yr = y + r * out;
    for (size_t o = 0; o < out; o++) {
      yr[o] = nn_dot(xr, w + o * in, in) + (b ? b[o] : 0.0f);
    }
  }
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

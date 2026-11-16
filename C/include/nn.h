#ifndef NN_H
#define NN_H

/*
 * nn - building blocks of a transformer, in float32, row-major, with no
 * libraries
 * Matrices are stored way PyTorch stores them: a linear layer with `in` inputs
 * and `out` outputs has a weight of out rows of `in`
 */

#include "nl_types.h"

// y[n][out] = x[n][in] . w[out][in]^T + b[out]   (b may be NULL)
void nn_linear(float *y, const float *x, size_t n, const float *w,
               const float *b, size_t in, size_t out);

// y = (x - mean) / sqrt(var + eps), per row of dim, then * g[i] and + b[i] if
// those are given. y may be x
void nn_layernorm(float *y, const float *x, size_t n, size_t dim, float eps,
                  const float *g, const float *b);

void nn_gelu(float *x, size_t n); // exact (erf) form
void nn_silu(float *x, size_t n);
void nn_softmax(float *x, size_t n);

// Dot product
float nn_dot(const float *a, const float *b, size_t n);

/*
 * Attention of one query row over keys and values that sit in rows of `stride`
 * floats (heads * head_dim wide), row r at index (first + r) wrapped by `ring`
 * if it is not 0. Head h uses columns h*hd .. h*hd+hd. out is heads * hd
 * floats
 * scratch holds nk floats
 */
void nn_attend(float *out, const float *q, const float *k, const float *v,
               size_t stride, size_t ring, size_t first, size_t nk,
               size_t heads, size_t hd, float *scratch);

// Rotary embedding of first `rot` dims of each of `heads` heads of x at
// position pos, interleaved pairs (2i, 2i+1), frequencies theta^(-2i/rot)
void nn_rope(float *x, size_t heads, size_t hd, size_t rot, size_t pos,
             float theta);

#endif

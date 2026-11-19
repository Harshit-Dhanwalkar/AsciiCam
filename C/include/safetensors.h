#ifndef SAFETENSORS_H
#define SAFETENSORS_H

/*
 * safetensors - reader for safetensors weights file format
 *
 * 8 bytes little endian header length N, N bytes of JSON naming every tensor
 * (dtype, shape, byte range), then raw data. No code is stored in it, which is
 * why it is format to trust File is untrusted all same: every length, range and
 * count is checked
 */

#include "nl_types.h"

#define ST_NAME 128
#define ST_MAX_TENSORS 4096
#define ST_MAX_DIMS 6
#define ST_MAX_HEADER (64u << 20u)

typedef enum { ST_F32, ST_F16, ST_BF16, ST_OTHER } st_dtype_t;

typedef struct {
  char name[ST_NAME];
  st_dtype_t dtype;
  int ndim;
  size_t shape[ST_MAX_DIMS];
  size_t numel;
  size_t off, len; // byte range in data section
  float *f32;      // converted copy, made on demand
} st_tensor_t;

typedef struct {
  unsigned char *buf; // whole file, or caller's memory
  size_t size;
  int own; // buf is ours to free
  size_t data_off;
  st_tensor_t *t;
  int n;
} st_t;

// Whole of a file in a new buffer (nl_free it), its size in *size; NULL with a
// reason in err when it can not be read
unsigned char *st_read_file(const char *path, size_t *size, char *err,
                            size_t err_size);

// Read file
// Return 0 on success, else -1 with a reason in err
int st_open(st_t *st, const char *path, char *err, size_t err_size);

// Parse a file already in memory (not copied; must outlive st). For tests
int st_parse(st_t *st, const unsigned char *buf, size_t size, char *err,
             size_t err_size);

void st_close(st_t *st);

const st_tensor_t *st_find(const st_t *st, const char *name);

// Tensor as float: points into file for F32, a converted copy for F16 and BF16
// (freed by st_close)
// Return NULL for other types
const float *st_f32(const st_t *st, const st_tensor_t *t);

// Convert one half precision value
float st_f16_to_f32(unsigned short h);

#endif

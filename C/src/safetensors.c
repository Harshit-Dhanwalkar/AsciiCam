/*
 * src/safetensors.c
 */

#include "platform.h"

#include "nolibc.h"

#include "safetensors.h"

#include <stdint.h>

static void fail(char *err, size_t size, const char *msg) {
  if (err && size) {
    nl_strncpy_safe(err, msg, size);
  }
}

float st_f16_to_f32(unsigned short h) {
  uint32_t sign = ((uint32_t)h & 0x8000u) << 16u;
  uint32_t exp = ((uint32_t)h >> 10u) & 0x1fu;
  uint32_t man = (uint32_t)h & 0x3ffu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else { // subnormal half: normalise
      exp = 127 - 15 + 1;
      while ((man & 0x400u) == 0) {
        man <<= 1u;
        exp--;
      }

      man &= 0x3ffu;
      bits = sign | (exp << 23u) | (man << 13u);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (man << 13u);
  } else {
    bits = sign | ((exp + 127 - 15) << 23u) | (man << 13u);
  }

  union {
    uint32_t u;
    float f;
  } v = {bits};

  return v.f;
}

/* A small JSON reader for exactly this header: an object whose values are
 * objects of dtype, shape and data_offsets, plus "__metadata__" */

typedef struct {
  const unsigned char *p, *end;
  const char *err;
} jr_t;

static void jr_ws(jr_t *j) {
  while (j->p < j->end &&
         (*j->p == ' ' || *j->p == '\n' || *j->p == '\t' || *j->p == '\r')) {
    j->p++;
  }
}

static int jr_str(jr_t *j, char *out, size_t size) {
  jr_ws(j);
  if (j->p >= j->end || *j->p != '"') {
    j->err = "string expected";

    return -1;
  }

  j->p++;
  size_t n = 0;
  while (j->p < j->end && *j->p != '"') {
    unsigned char c = *j->p++;
    if (c == '\\') {
      if (j->p >= j->end) {
        break;
      }

      c = *j->p++;
      if (c == 'u') { // only plain ASCII names occur; keep escape out
        j->err = "unicode escape in a name";

        return -1;
      }

      if (c == 'n') {
        c = '\n';
      } else if (c == 't') {
        c = '\t';
      }
    }

    if (c < 0x20 || c >= 0x7f) {
      j->err = "non ASCII or control character in a name";

      return -1;
    }

    if (n + 1 < size) {
      out[n] = (char)c;
    }

    n++;
  }

  if (j->p >= j->end) {
    j->err = "unterminated string";

    return -1;
  }

  j->p++;
  if (n + 1 > size) {
    j->err = "name too long";

    return -1;
  }

  out[n] = 0;

  return 0;
}

static int jr_uint(jr_t *j, size_t *out) {
  jr_ws(j);
  if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
    j->err = "number expected";

    return -1;
  }

  size_t v = 0;
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
    size_t d = (size_t)(*j->p - '0');
    if (v > (SIZE_MAX - d) / 10) {
      j->err = "number too large";

      return -1;
    }

    v = v * 10 + d;
    j->p++;
  }

  *out = v;

  return 0;
}

static int jr_expect(jr_t *j, char c) {
  jr_ws(j);
  if (j->p >= j->end || *j->p != (unsigned char)c) {
    j->err = "unexpected character in header";

    return -1;
  }

  j->p++;

  return 0;
}

// Skip any JSON value (used for __metadata__ and unknown keys)
static int jr_skip(jr_t *j, int depth) {
  if (depth > 16) {
    j->err = "nesting too deep";

    return -1;
  }

  jr_ws(j);
  if (j->p >= j->end) {
    j->err = "value expected";

    return -1;
  }

  unsigned char c = *j->p;
  if (c == '"') {
    char tmp[2];
    // a string of any length: walk it without storing
    j->p++;
    while (j->p < j->end && *j->p != '"') {
      if (*j->p == '\\') {
        j->p++;
      }

      j->p++;
    }

    (void)tmp;
    if (j->p >= j->end) {
      j->err = "unterminated string";

      return -1;
    }

    j->p++;

    return 0;
  }

  if (c == '{' || c == '[') {
    unsigned char close = c == '{' ? '}' : ']';
    j->p++;
    jr_ws(j);
    if (j->p < j->end && *j->p == close) {
      j->p++;

      return 0;
    }

    for (;;) {
      if (c == '{') {
        if (jr_skip(j, depth + 1) != 0 || jr_expect(j, ':') != 0) {
          return -1;
        }
      }

      if (jr_skip(j, depth + 1) != 0) {
        return -1;
      }

      jr_ws(j);
      if (j->p < j->end && *j->p == ',') {
        j->p++;
        continue;
      }

      return jr_expect(j, (char)close);
    }
  }

  // number, true, false, null
  size_t n = 0;
  while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
         *j->p != ' ' && *j->p != '\n') {
    j->p++;
    n++;
  }

  if (n == 0) {
    j->err = "value expected";

    return -1;
  }

  return 0;
}

static int dtype_of(const char *s, st_dtype_t *d, size_t *bytes) {
  if (nl_strcmp(s, "F32") == 0) {
    *d = ST_F32;
    *bytes = 4;
  } else if (nl_strcmp(s, "F16") == 0) {
    *d = ST_F16;
    *bytes = 2;
  } else if (nl_strcmp(s, "BF16") == 0) {
    *d = ST_BF16;
    *bytes = 2;
  } else {
    *d = ST_OTHER;
    *bytes = 0; // length is not checked against shape
  }

  return 0;
}

static int parse_tensor(jr_t *j, st_tensor_t *t, size_t data_size) {
  char key[32];
  char dt[16] = {0};
  int have_dtype = 0, have_shape = 0, have_off = 0;
  size_t s = 0, e = 0;

  if (jr_expect(j, '{') != 0) {
    return -1;
  }

  jr_ws(j);
  if (j->p < j->end && *j->p == '}') {
    j->err = "empty tensor entry";

    return -1;
  }

  for (;;) {
    if (jr_str(j, key, sizeof(key)) != 0 || jr_expect(j, ':') != 0) {
      return -1;
    }

    if (nl_strcmp(key, "dtype") == 0) {
      if (jr_str(j, dt, sizeof(dt)) != 0) {
        return -1;
      }

      have_dtype = 1;
    } else if (nl_strcmp(key, "shape") == 0) {
      if (jr_expect(j, '[') != 0) {
        return -1;
      }

      t->ndim = 0;
      t->numel = 1;
      jr_ws(j);
      if (j->p < j->end && *j->p == ']') {
        j->p++;
      } else {
        for (;;) {
          size_t d;
          if (jr_uint(j, &d) != 0) {
            return -1;
          }

          if (t->ndim >= ST_MAX_DIMS) {
            j->err = "too many dimensions";

            return -1;
          }

          if (d != 0 && t->numel > SIZE_MAX / d) {
            j->err = "shape too large";

            return -1;
          }

          t->shape[t->ndim++] = d;
          t->numel *= d;
          jr_ws(j);
          if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
          }

          if (jr_expect(j, ']') != 0) {
            return -1;
          }

          break;
        }
      }

      have_shape = 1;
    } else if (nl_strcmp(key, "data_offsets") == 0) {
      if (jr_expect(j, '[') != 0 || jr_uint(j, &s) != 0 ||
          jr_expect(j, ',') != 0 || jr_uint(j, &e) != 0 ||
          jr_expect(j, ']') != 0) {
        return -1;
      }

      have_off = 1;
    } else if (jr_skip(j, 1) != 0) {
      return -1;
    }

    jr_ws(j);
    if (j->p < j->end && *j->p == ',') {
      j->p++;
      continue;
    }

    if (jr_expect(j, '}') != 0) {
      return -1;
    }

    break;
  }

  if (!have_dtype || !have_shape || !have_off) {
    j->err = "tensor entry misses dtype, shape or data_offsets";

    return -1;
  }

  size_t bytes;
  dtype_of(dt, &t->dtype, &bytes);
  if (e < s || e > data_size) {
    j->err = "tensor data outside the file";

    return -1;
  }

  t->off = s;
  t->len = e - s;
  if (bytes != 0) {
    if (t->numel > SIZE_MAX / bytes || t->numel * bytes != t->len) {
      j->err = "tensor size does not match its shape";

      return -1;
    }
  }

  return 0;
}

int st_parse(st_t *st, const unsigned char *buf, size_t size, char *err,
             size_t err_size) {
  nl_memset(st, 0, sizeof(*st));
  st->buf = (unsigned char *)buf;
  st->size = size;

  if (size < 8) {
    fail(err, err_size, "file shorter than the 8 byte header length");

    return -1;
  }

  uint64_t hl = 0;
  for (int i = 7; i >= 0; i--) {
    hl = hl << 8u | buf[i];
  }

  if (hl > ST_MAX_HEADER || hl > size - 8) {
    fail(err, err_size, "header length is larger than the file");

    return -1;
  }

  size_t data_off = 8 + (size_t)hl;
  size_t data_size = size - data_off;
  st->data_off = data_off;

  st->t = nl_calloc(ST_MAX_TENSORS, sizeof(st_tensor_t));
  if (!st->t) {
    fail(err, err_size, "out of memory");

    return -1;
  }

  jr_t j = {buf + 8, buf + data_off, NULL};
  char name[ST_NAME];
  if (jr_expect(&j, '{') != 0) {
    fail(err, err_size, "header is not a JSON object");
    st_close(st);

    return -1;
  }

  jr_ws(&j);
  int empty = j.p < j.end && *j.p == '}';
  if (empty) {
    j.p++;
  }

  while (!empty) {
    if (jr_str(&j, name, sizeof(name)) != 0 || jr_expect(&j, ':') != 0) {
      fail(err, err_size, j.err ? j.err : "bad header");
      st_close(st);

      return -1;
    }

    if (nl_strcmp(name, "__metadata__") == 0) {
      if (jr_skip(&j, 0) != 0) {
        fail(err, err_size, j.err ? j.err : "bad metadata");
        st_close(st);

        return -1;
      }
    } else {
      if (st->n >= ST_MAX_TENSORS) {
        fail(err, err_size, "too many tensors");
        st_close(st);

        return -1;
      }

      st_tensor_t *t = &st->t[st->n];
      nl_strncpy_safe(t->name, name, sizeof(t->name));
      if (parse_tensor(&j, t, data_size) != 0) {
        fail(err, err_size, j.err ? j.err : "bad tensor entry");
        st_close(st);

        return -1;
      }

      for (int i = 0; i < st->n; i++) {
        if (nl_strcmp(st->t[i].name, name) == 0) {
          fail(err, err_size, "a tensor name occurs twice");
          st_close(st);

          return -1;
        }
      }

      st->n++;
    }

    jr_ws(&j);
    if (j.p < j.end && *j.p == ',') {
      j.p++;
      continue;
    }

    if (jr_expect(&j, '}') != 0) {
      fail(err, err_size, "header object is not closed");
      st_close(st);

      return -1;
    }

    break;
  }

  jr_ws(&j);
  if (j.p != j.end) {
    fail(err, err_size, "text after the header object");
    st_close(st);

    return -1;
  }

  return 0;
}

unsigned char *st_read_file(const char *path, size_t *size, char *err,
                            size_t err_size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fail(err, err_size, "cannot open the file");

    return NULL;
  }

  size_t cap = 1u << 20u, n = 0;
  unsigned char *buf = nl_malloc(cap);
  while (buf) {
    if (n == cap) {
      if (cap > ((size_t)1 << 34u)) {
        break;
      }

      unsigned char *nb = nl_malloc(cap * 2);
      if (!nb) {
        nl_free(buf);
        buf = NULL;
        break;
      }

      nl_memcpy(nb, buf, n);
      nl_free(buf);
      buf = nb;
      cap *= 2;
    }

    ssize_t r = read(fd, buf + n, cap - n);
    if (r <= 0) {
      if (r < 0) {
        nl_free(buf);
        buf = NULL;
      }

      break;
    }

    n += (size_t)r;
  }

  close(fd);
  if (!buf) {
    fail(err, err_size, "cannot read the file");

    return NULL;
  }

  *size = n;

  return buf;
}

int st_open(st_t *st, const char *path, char *err, size_t err_size) {
  nl_memset(st, 0, sizeof(*st));
  size_t n = 0;
  unsigned char *buf = st_read_file(path, &n, err, err_size);
  if (!buf) {
    return -1;
  }

  if (st_parse(st, buf, n, err, err_size) != 0) {
    nl_free(buf);
    nl_memset(st, 0, sizeof(*st));

    return -1;
  }

  st->own = 1;

  return 0;
}

void st_close(st_t *st) {
  if (st->t) {
    for (int i = 0; i < st->n; i++) {
      nl_free(st->t[i].f32);
    }

    nl_free(st->t);
  }

  if (st->own) {
    nl_free(st->buf);
  }

  nl_memset(st, 0, sizeof(*st));
}

const st_tensor_t *st_find(const st_t *st, const char *name) {
  for (int i = 0; i < st->n; i++) {
    if (nl_strcmp(st->t[i].name, name) == 0) {
      return &st->t[i];
    }
  }

  return NULL;
}

const float *st_f32(const st_t *st, const st_tensor_t *t) {
  const unsigned char *src = st->buf + st->data_off + t->off;
  st_tensor_t *w = (st_tensor_t *)t; // the cache field is the only one set

  if (t->dtype == ST_F32) {
    if (((uintptr_t)src & 3u) == 0) {
      return (const float *)src;
    }

    if (!w->f32) {
      w->f32 = nl_malloc(t->len ? t->len : 4);
      if (w->f32) {
        nl_memcpy(w->f32, src, t->len);
      }
    }

    return w->f32;
  }

  if (t->dtype != ST_F16 && t->dtype != ST_BF16) {
    return NULL;
  }

  if (!w->f32) {
    w->f32 = nl_malloc(t->numel * sizeof(float) + 4);
    if (!w->f32) {
      return NULL;
    }

    for (size_t i = 0; i < t->numel; i++) {
      unsigned short h = (unsigned short)((unsigned)src[2 * i] |
                                          (unsigned)src[2 * i + 1] << 8u);
      if (t->dtype == ST_F16) {
        w->f32[i] = st_f16_to_f32(h);
      } else {
        union {
          uint32_t u;
          float f;
        } v = {(uint32_t)h << 16u};
        w->f32[i] = v.f;
      }
    }
  }

  return w->f32;
}

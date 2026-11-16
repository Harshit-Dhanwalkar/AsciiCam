#include "asrnet.h"
#include "nolibc.h"

/*
 * config.json of model -> an_config_t
 * Keys that are missing keep tiny model's value; ones that decide shape of
 * network (sizes, layers, heads, windows) are checked against what engine can
 * hold
 * A small walker, no general JSON: values are found by path
 */

typedef struct {
  const char *p, *end;
} cur_t;

static void ws(cur_t *c) {
  while (c->p < c->end &&
         (*c->p == ' ' || *c->p == '\n' || *c->p == '\t' || *c->p == '\r')) {
    c->p++;
  }
}

// A string at c into out (no escapes kept: keys here are plain); -1 on error
static int str(cur_t *c, char *out, size_t cap) {
  if (c->p >= c->end || *c->p != '"') {
    return -1;
  }

  c->p++;
  size_t n = 0;
  while (c->p < c->end && *c->p != '"') {
    if (*c->p == '\\') {
      c->p++;
      if (c->p >= c->end) {
        return -1;
      }
    }

    if (out && n + 1 < cap) {
      out[n] = *c->p;
    }

    n++;
    c->p++;
  }

  if (c->p >= c->end) {
    return -1;
  }

  c->p++;
  if (out) {
    out[n < cap ? n : cap - 1] = 0;
  }

  return n < cap ? 0 : -1;
}

static int skip(cur_t *c, int depth) {
  if (depth > 16) {
    return -1;
  }

  ws(c);
  if (c->p >= c->end) {
    return -1;
  }

  if (*c->p == '"') {
    return str(c, NULL, 0) == -1 && c->p >= c->end ? -1 : 0;
  }

  if (*c->p == '{' || *c->p == '[') {
    char close = *c->p == '{' ? '}' : ']';
    c->p++;
    ws(c);
    if (c->p < c->end && *c->p == close) {
      c->p++;
      return 0;
    }

    for (;;) {
      ws(c);
      if (close == '}') {
        if (str(c, NULL, 0) != 0 && c->p >= c->end) {
          return -1;
        }

        ws(c);
        if (c->p >= c->end || *c->p != ':') {
          return -1;
        }

        c->p++;
      }

      if (skip(c, depth + 1)) {
        return -1;
      }

      ws(c);
      if (c->p >= c->end) {
        return -1;
      }

      if (*c->p == ',') {
        c->p++;
        continue;
      }

      if (*c->p == close) {
        c->p++;
        return 0;
      }

      return -1;
    }
  }

  while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
         *c->p != ' ' && *c->p != '\n' && *c->p != '\r' && *c->p != '\t') {
    c->p++;
  }

  return 0;
}

// Inside the object at c (on its '{'), move to the value of key. 1 found,
// 0 absent, -1 malformed
static int enter(cur_t *c, const char *key) {
  ws(c);
  if (c->p >= c->end || *c->p != '{') {
    return -1;
  }

  c->p++;
  for (;;) {
    char k[64];
    ws(c);
    if (c->p < c->end && *c->p == ',') {
      c->p++;
      continue;
    }

    if (c->p >= c->end) {
      return -1;
    }

    if (*c->p == '}') {
      return 0;
    }

    int r = str(c, k, sizeof(k));
    if (r != 0) {
      // a key longer than the buffer is skipped over, never matched
      if (c->p >= c->end) {
        return -1;
      }

      k[0] = 0;
    }

    ws(c);
    if (c->p >= c->end || *c->p != ':') {
      return -1;
    }

    c->p++;
    ws(c);
    if (nl_strcmp(k, key) == 0) {
      return 1;
    }

    if (skip(c, 0)) {
      return -1;
    }
  }
}

// A number at c: integer part, and fraction as double (no exponent needed)
static int number(cur_t *c, double *out) {
  ws(c);
  const char *p = c->p;
  int neg = 0;
  if (p < c->end && *p == '-') {
    neg = 1;
    p++;
  }

  if (p >= c->end || *p < '0' || *p > '9') {
    return -1;
  }

  double v = 0;
  while (p < c->end && *p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 1e12) {
      return -1;
    }

    p++;
  }

  if (p < c->end && *p == '.') {
    p++;
    double f = 0.1;
    while (p < c->end && *p >= '0' && *p <= '9') {
      v += f * (*p - '0');
      f /= 10;
      p++;
    }
  }

  if (p < c->end && (*p == 'e' || *p == 'E')) {
    return -1;
  }

  c->p = p;
  *out = neg ? -v : v;

  return 0;
}

// Value of a number at path (0 to 2 keys deep) in the document. 1 found,
// 0 absent or null, -1 malformed or not a number
static int get(const char *json, size_t n, const char *k1, const char *k2,
               double *out) {
  cur_t c = {json, json + n};
  int r = enter(&c, k1);
  if (r != 1) {
    return r;
  }

  if (k2) {
    r = enter(&c, k2);
    if (r != 1) {
      return r;
    }
  }

  ws(&c);
  if (c.end - c.p >= 4 && c.p[0] == 'n' && c.p[1] == 'u') {
    return 0;
  }

  return number(&c, out) == 0 ? 1 : -1;
}

static int to_int(double v, int lo, int hi, int *out) {
  if (v < lo || v > hi || v != (double)(int)v) {
    return -1;
  }

  *out = (int)v;

  return 0;
}

static int set_int(const char *j, size_t n, const char *k1, const char *k2,
                   int lo, int hi, int *dst, char *err, size_t es) {
  double v = 0;
  int r = get(j, n, k1, k2, &v);
  if (r == 0) {
    return 0;
  }

  if (r < 0 || to_int(v, lo, hi, dst) != 0) {
    nl_snprintf(err, es, "config.json: bad value for %s", k2 ? k2 : k1);

    return -1;
  }

  return 0;
}

int an_config_parse(an_config_t *c, const char *json, size_t n, char *err,
                    size_t es) {
  an_config_tiny(c);
  if (n == 0 || n > (1u << 20u)) {
    nl_strncpy_safe(err, "config.json: empty or too large", es);

    return -1;
  }

  cur_t probe = {json, json + n};
  ws(&probe);
  if (skip(&probe, 0) != 0) {
    nl_strncpy_safe(err, "config.json is not valid JSON", es);

    return -1;
  }

  ws(&probe);
  if (probe.p != probe.end) {
    nl_strncpy_safe(err, "config.json has trailing bytes", es);

    return -1;
  }

  int bos = c->start_tok;
  int rc = 0;
  // Decoder
  rc += set_int(json, n, "vocab_size", NULL, 2, 1048576, &c->vocab, err, es);
  rc += set_int(json, n, "hidden_size", NULL, 1, 8192, &c->dhid, err, es);
  rc +=
      set_int(json, n, "intermediate_size", NULL, 1, 65536, &c->dffn, err, es);
  rc += set_int(json, n, "num_hidden_layers", NULL, 1, AN_MAX_LAYERS,
                &c->dlayers, err, es);
  rc += set_int(json, n, "num_attention_heads", NULL, 1, 256, &c->dheads, err,
                es);
  rc += set_int(json, n, "bos_token_id", NULL, 0, 1048576, &bos, err, es);
  rc +=
      set_int(json, n, "eos_token_id", NULL, 0, 1048576, &c->eos_tok, err, es);
  c->start_tok = bos;
  rc += set_int(json, n, "decoder_start_token_id", NULL, 0, 1048576,
                &c->start_tok, err, es);
  // encoder
  rc += set_int(json, n, "encoder_config", "hidden_size", 1, 8192, &c->hid, err,
                es);
  rc += set_int(json, n, "encoder_config", "intermediate_size", 1, 65536,
                &c->ffn, err, es);
  rc += set_int(json, n, "encoder_config", "num_hidden_layers", 1,
                AN_MAX_LAYERS, &c->layers, err, es);
  rc += set_int(json, n, "encoder_config", "num_attention_heads", 1, 256,
                &c->heads, err, es);
  if (rc) {
    return -1;
  }

  double v;
  if (get(json, n, "rope_parameters", "rope_theta", &v) == 1 && v > 0) {
    c->rope_theta = (float)v;
  }

  if (get(json, n, "rope_parameters", "partial_rotary_factor", &v) == 1 &&
      v > 0 && v <= 1) {
    c->rope_partial = (float)v;
  }

  // the encoder's windows: [[left, right], ...], one per layer
  cur_t w = {json, json + n};
  if (enter(&w, "encoder_config") == 1 && enter(&w, "sliding_windows") == 1) {
    ws(&w);
    if (w.p >= w.end || *w.p != '[') {
      nl_strncpy_safe(err, "config.json: sliding_windows is not a list", es);

      return -1;
    }

    w.p++;
    int i = 0;
    for (;;) {
      ws(&w);
      if (w.p < w.end && *w.p == ']') {
        break;
      }

      if (i >= AN_MAX_LAYERS || w.p >= w.end || *w.p != '[') {
        nl_strncpy_safe(err, "config.json: bad sliding_windows", es);

        return -1;
      }

      w.p++;
      double l, r;
      ws(&w);
      if (number(&w, &l) != 0) {
        nl_strncpy_safe(err, "config.json: bad sliding_windows", es);

        return -1;
      }

      ws(&w);
      if (w.p >= w.end || *w.p != ',') {
        nl_strncpy_safe(err, "config.json: bad sliding_windows", es);

        return -1;
      }

      w.p++;
      ws(&w);
      if (number(&w, &r) != 0) {
        nl_strncpy_safe(err, "config.json: bad sliding_windows", es);

        return -1;
      }

      ws(&w);
      if (w.p >= w.end || *w.p != ']' || to_int(l, 1, AN_RING, &c->win_l[i]) ||
          to_int(r, 0, AN_RING / 2, &c->win_r[i])) {
        nl_strncpy_safe(err, "config.json: window out of range", es);

        return -1;
      }

      w.p++;
      i++;
      ws(&w);
      if (w.p < w.end && *w.p == ',') {
        w.p++;
      }
    }

    if (i != c->layers) {
      nl_strncpy_safe(err, "config.json: sliding_windows and layers differ",
                      es);

      return -1;
    }
  }

  if (c->hid % c->heads || c->dhid % c->dheads) {
    nl_strncpy_safe(err, "config.json: hidden size not divisible by heads", es);

    return -1;
  }

  return 0;
}

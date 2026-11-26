#include "nolibc.h"

#include "tokdec.h"

static void fail(char *err, size_t size, const char *msg) {
  if (err && size) {
    nl_strncpy_safe(err, msg, size);
  }
}

typedef struct {
  const char *p, *end;
} cur_t;

static void skip_ws(cur_t *c) {
  while (c->p < c->end &&
         (*c->p == ' ' || *c->p == '\n' || *c->p == '\t' || *c->p == '\r')) {
    c->p++;
  }
}

static int hexv(int ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }

  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }

  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }

  return -1;
}

static size_t put_utf8(unsigned cp, char *out) {
  if (cp < 0x80u) {
    out[0] = (char)cp;
    return 1;
  }

  if (cp < 0x800u) {
    out[0] = (char)(0xC0u | (cp >> 6u));
    out[1] = (char)(0x80u | (cp & 0x3Fu));
    return 2;
  }

  if (cp < 0x10000u) {
    out[0] = (char)(0xE0u | (cp >> 12u));
    out[1] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
  }

  out[0] = (char)(0xF0u | (cp >> 18u));
  out[1] = (char)(0x80u | ((cp >> 12u) & 0x3Fu));
  out[2] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
  out[3] = (char)(0x80u | (cp & 0x3Fu));
  return 4;
}

// A JSON string at c (opening quote), unescaped into out (cap bytes, NUL
// added). Returns its length, -1 when malformed or too long
static int read_string(cur_t *c, char *out, size_t cap) {
  if (c->p >= c->end || *c->p != '"') {
    return -1;
  }

  c->p++;
  size_t n = 0;
  while (c->p < c->end && *c->p != '"') {
    unsigned char ch = (unsigned char)*c->p++;
    char tmp[4];
    size_t k = 1;
    if (ch < 0x20u) {
      return -1;
    }

    if (ch == '\\') {
      if (c->p >= c->end) {
        return -1;
      }

      char e = *c->p++;
      if (e == 'n') {
        tmp[0] = '\n';
      } else if (e == 't') {
        tmp[0] = '\t';
      } else if (e == 'r') {
        tmp[0] = '\r';
      } else if (e == 'b') {
        tmp[0] = '\b';
      } else if (e == 'f') {
        tmp[0] = '\f';
      } else if (e == '"' || e == '\\' || e == '/') {
        tmp[0] = e;
      } else if (e == 'u') {
        unsigned cp = 0;
        for (int i = 0; i < 4; i++) {
          if (c->p >= c->end || hexv(*c->p) < 0) {
            return -1;
          }

          cp = cp << 4u | (unsigned)hexv(*c->p++);
        }

        if (cp >= 0xD800u && cp < 0xDC00u) { // a pair of surrogates
          if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u') {
            return -1;
          }

          unsigned lo = 0;
          for (int i = 0; i < 4; i++) {
            int v = hexv(c->p[2 + i]);
            if (v < 0) {
              return -1;
            }

            lo = lo << 4u | (unsigned)v;
          }

          if (lo < 0xDC00u || lo > 0xDFFFu) {
            return -1;
          }

          c->p += 6;
          cp = 0x10000u + ((cp - 0xD800u) << 10u) + (lo - 0xDC00u);
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
          return -1;
        }

        k = put_utf8(cp, tmp);
      } else {
        return -1;
      }
    } else {
      tmp[0] = (char)ch;
    }

    if (n + k >= cap) {
      return -1;
    }

    for (size_t i = 0; i < k; i++) {
      out[n++] = tmp[i];
    }
  }

  if (c->p >= c->end) {
    return -1;
  }

  c->p++;
  out[n] = 0;
  return (int)n;
}

// A non-negative integer
static long read_int(cur_t *c) {
  if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
    return -1;
  }

  long v = 0;
  while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
    v = v * 10 + (*c->p - '0');
    if (v > 100000000L) {
      return -1;
    }

    c->p++;
  }

  return v;
}

// Skip one JSON value of any kind; 0 on success. Depth is bounded
static int skip_value(cur_t *c, int depth) {
  if (depth > 32) {
    return -1;
  }

  skip_ws(c);
  if (c->p >= c->end) {
    return -1;
  }

  if (*c->p == '"') {
    // long strings (normalizer regexes) are skipped without storing
    c->p++;
    while (c->p < c->end && *c->p != '"') {
      if (*c->p == '\\') {
        c->p++;
      }

      c->p++;
    }

    if (c->p >= c->end) {
      return -1;
    }

    c->p++;
    return 0;
  }

  if (*c->p == '{' || *c->p == '[') {
    char close = *c->p == '{' ? '}' : ']';
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == close) {
      c->p++;
      return 0;
    }

    for (;;) {
      skip_ws(c);
      if (close == '}') {
        if (c->p >= c->end || *c->p != '"') {
          return -1;
        }

        c->p++;
        while (c->p < c->end && *c->p != '"') {
          if (*c->p == '\\') {
            c->p++;
          }

          c->p++;
        }

        if (c->p >= c->end) {
          return -1;
        }

        c->p++;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
          return -1;
        }

        c->p++;
      }

      if (skip_value(c, depth + 1)) {
        return -1;
      }

      skip_ws(c);
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

  // number, true, false, null
  while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
         *c->p != ' ' && *c->p != '\n' && *c->p != '\r' && *c->p != '\t') {
    c->p++;
  }

  return 0;
}

typedef struct {
  tokdec_t *t;
  size_t pool_used;
} build_t;

static int store(build_t *b, long id, const char *s, size_t len, int special) {
  if (id < 0 || id >= TD_MAX_IDS) {
    return -1;
  }

  if (b->pool_used + len + 1 > TD_MAX_POOL) {
    return -1;
  }

  if (b->t->piece[id] && !special && !b->t->special[id]) {
    return -1; // two tokens with one id
  }

  char *d = b->t->pool + b->pool_used;
  nl_memcpy(d, s, len);
  d[len] = 0;
  b->pool_used += len + 1;
  b->t->piece[id] = d;
  if (special) {
    b->t->special[id] = 1;
  }

  if (id >= b->t->n) {
    b->t->n = (int)id + 1;
  }

  return 0;
}

// {"piece": id, ...}
static int read_vocab(build_t *b, cur_t *c) {
  skip_ws(c);
  if (c->p >= c->end || *c->p != '{') {
    return -1;
  }

  c->p++;
  skip_ws(c);
  if (c->p < c->end && *c->p == '}') {
    c->p++;
    return 0;
  }

  for (;;) {
    char piece[TD_MAX_PIECE * 4];
    skip_ws(c);
    int len = read_string(c, piece, sizeof(piece));
    if (len < 0) {
      return -1;
    }

    skip_ws(c);
    if (c->p >= c->end || *c->p != ':') {
      return -1;
    }

    c->p++;
    skip_ws(c);
    long id = read_int(c);
    if (store(b, id, piece, (size_t)len, 0)) {
      return -1;
    }

    skip_ws(c);
    if (c->p < c->end && *c->p == ',') {
      c->p++;
      continue;
    }

    if (c->p < c->end && *c->p == '}') {
      c->p++;
      return 0;
    }

    return -1;
  }
}

// [{"id": 1, "content": "<s>", "special": true}, ...]
static int read_added(build_t *b, cur_t *c) {
  skip_ws(c);
  if (c->p >= c->end || *c->p != '[') {
    return -1;
  }

  c->p++;
  skip_ws(c);
  if (c->p < c->end && *c->p == ']') {
    c->p++;
    return 0;
  }

  for (;;) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '{') {
      return -1;
    }

    c->p++;
    long id = -1;
    int special = 0, have = 0;
    char content[TD_MAX_PIECE * 4];
    content[0] = 0;
    size_t clen = 0;
    for (;;) {
      char key[32];
      skip_ws(c);
      if (c->p < c->end && *c->p == '}') {
        break;
      }

      if (read_string(c, key, sizeof(key)) < 0) {
        return -1;
      }

      skip_ws(c);
      if (c->p >= c->end || *c->p != ':') {
        return -1;
      }

      c->p++;
      skip_ws(c);
      if (nl_strcmp(key, "id") == 0) {
        id = read_int(c);
      } else if (nl_strcmp(key, "content") == 0) {
        int n = read_string(c, content, sizeof(content));
        if (n < 0) {
          return -1;
        }

        clen = (size_t)n;
        have = 1;
      } else if (nl_strcmp(key, "special") == 0) {
        special = c->end - c->p >= 4 && c->p[0] == 't' && c->p[1] == 'r' &&
                  c->p[2] == 'u' && c->p[3] == 'e';
        if (skip_value(c, 0)) {
          return -1;
        }
      } else if (skip_value(c, 0)) {
        return -1;
      }

      skip_ws(c);
      if (c->p < c->end && *c->p == ',') {
        c->p++;
      }
    }

    c->p++; // }
    if (!have || id < 0) {
      return -1;
    }

    // a control token prints nothing; an ordinary added word prints itself
    if (store(b, id, content, clen, 1)) {
      return -1;
    }

    if (!special) {
      b->t->special[id] = 0;
    }

    skip_ws(c);
    if (c->p < c->end && *c->p == ',') {
      c->p++;
      continue;
    }

    if (c->p < c->end && *c->p == ']') {
      c->p++;
      return 0;
    }

    return -1;
  }
}

// Find `"key":` directly inside object at c (c just after its '{') and leave c
// at value
// Return 0 when found
static int find_key(cur_t *c, const char *want) {
  for (;;) {
    char key[64];
    skip_ws(c);
    if (c->p >= c->end || *c->p == '}') {
      return -1;
    }

    if (*c->p == ',') {
      c->p++;
      continue;
    }

    int n = read_string(c, key, sizeof(key));
    if (n < 0) {
      // a key too long for buffer: skip it
      return -1;
    }

    skip_ws(c);
    if (c->p >= c->end || *c->p != ':') {
      return -1;
    }

    c->p++;
    skip_ws(c);
    if (nl_strcmp(key, want) == 0) {
      return 0;
    }

    if (skip_value(c, 0)) {
      return -1;
    }
  }
}

int tokdec_parse(tokdec_t *t, const char *json, size_t size, char *err,
                 size_t err_size) {
  nl_memset(t, 0, sizeof(*t));
  cur_t top = {json, json + size};
  skip_ws(&top);
  if (top.p >= top.end || *top.p != '{') {
    fail(err, err_size, "tokenizer.json is not a JSON object");
    return -1;
  }

  top.p++;
  t->piece = (const char **)nl_calloc(TD_MAX_IDS, sizeof(*t->piece));
  t->special = nl_calloc(TD_MAX_IDS, 1);
  t->pool = nl_malloc(TD_MAX_POOL);
  build_t b = {t, 0};
  if (!t->piece || !t->special || !t->pool) {
    fail(err, err_size, "out of memory");
    tokdec_free(t);
    return -1;
  }

  int got_vocab = 0;
  // added_tokens comes before model in files seen so far, but do not depend on
  // it: walk top level keys
  cur_t c = top;
  for (;;) {
    char key[64];
    skip_ws(&c);
    if (c.p >= c.end || *c.p == '}') {
      break;
    }

    if (*c.p == ',') {
      c.p++;
      continue;
    }

    if (read_string(&c, key, sizeof(key)) < 0) {
      fail(err, err_size, "bad key in tokenizer.json");
      tokdec_free(t);
      return -1;
    }

    skip_ws(&c);
    if (c.p >= c.end || *c.p != ':') {
      fail(err, err_size, "bad tokenizer.json");
      tokdec_free(t);
      return -1;
    }

    c.p++;
    skip_ws(&c);
    if (nl_strcmp(key, "model") == 0 && c.p < c.end && *c.p == '{') {
      cur_t m = c;
      m.p++;
      if (find_key(&m, "vocab") == 0) {
        if (read_vocab(&b, &m)) {
          fail(err, err_size, "bad vocab in tokenizer.json");
          tokdec_free(t);
          return -1;
        }

        got_vocab = 1;
      }
    } else if (nl_strcmp(key, "added_tokens") == 0) {
      cur_t a = c;
      if (read_added(&b, &a)) {
        fail(err, err_size, "bad added_tokens in tokenizer.json");
        tokdec_free(t);
        return -1;
      }
    }

    if (skip_value(&c, 0)) {
      fail(err, err_size, "bad tokenizer.json");
      tokdec_free(t);
      return -1;
    }
  }

  if (c.p >= c.end) {
    fail(err, err_size, "tokenizer.json is cut short");
    tokdec_free(t);
    return -1;
  }

  if (!got_vocab || t->n < 2) {
    fail(err, err_size, "tokenizer.json has no vocabulary");
    tokdec_free(t);
    return -1;
  }

  return 0;
}

void tokdec_free(tokdec_t *t) {
  nl_free((void *)t->piece);
  nl_free(t->special);
  nl_free(t->pool);
  nl_memset(t, 0, sizeof(*t));
}

// <s> </s> <pad> <unk> print nothing even when tokenizer.json forgot to mark
// them special
static int control_piece(const char *s) {
  static const char *const names[] = {"<s>",   "</s>",  "<pad>",
                                      "<unk>", "<bos>", "<eos>"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (nl_strcmp(s, names[i]) == 0) {
      return 1;
    }
  }

  return 0;
}

static int byte_token(const char *s) {
  if (s[0] == '<' && s[1] == '0' && s[2] == 'x' && hexv(s[3]) >= 0 &&
      hexv(s[4]) >= 0 && s[5] == '>' && s[6] == 0) {
    return hexv(s[3]) * 16 + hexv(s[4]);
  }

  return -1;
}

int tokdec_is_text(const tokdec_t *t, int id) {
  return id >= 0 && id < t->n && t->piece[id] && !t->special[id] &&
         !control_piece(t->piece[id]);
}

size_t tokdec_append(const tokdec_t *t, int id, char *out, size_t out_size,
                     size_t at) {
  if (out_size == 0 || at >= out_size) {
    return 0;
  }

  if (id < 0 || id >= t->n || !t->piece[id] || t->special[id]) {
    out[at] = 0;
    return 0;
  }

  const char *s = t->piece[id];
  size_t n = 0;
  if (control_piece(s)) {
    out[at] = 0;

    return 0;
  }

  int b = byte_token(s);
  if (b >= 0) {
    if (at + 1 < out_size) {
      out[at + n++] = (char)b;
    }
  } else {
    while (*s && at + n + 1 < out_size) {
      // Marker U+2581 (E2 96 81) is a space
      if ((unsigned char)s[0] == 0xE2u && (unsigned char)s[1] == 0x96u &&
          (unsigned char)s[2] == 0x81u) {
        out[at + n++] = ' ';
        s += 3;
      } else {
        out[at + n++] = *s++;
      }
    }
  }

  out[at + n] = 0;
  return n;
}

// Length of valid UTF-8 character at s (n bytes left), 0 when invalid
static int utf8_len(const unsigned char *s, size_t n) {
  if (s[0] < 0x80u) {
    return 1;
  }

  int k = s[0] >= 0xF0u ? 4 : s[0] >= 0xE0u ? 3 : s[0] >= 0xC2u ? 2 : 0;
  if (s[0] > 0xF4u || k == 0 || (size_t)k > n) {
    return 0;
  }

  for (int i = 1; i < k; i++) {
    if ((s[i] & 0xC0u) != 0x80u) {
      return 0;
    }
  }

  unsigned cp = k == 2 ? s[0] & 0x1Fu : k == 3 ? s[0] & 0x0Fu : s[0] & 0x07u;
  for (int i = 1; i < k; i++) {
    cp = cp << 6u | (s[i] & 0x3Fu);
  }

  if ((k == 3 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) ||
      (k == 4 && (cp < 0x10000u || cp > 0x10FFFFu))) {
    return 0;
  }

  return k;
}

size_t tokdec_text(const tokdec_t *t, const int *ids, int n, char *out,
                   size_t out_size) {
  if (out_size == 0) {
    return 0;
  }

  size_t at = 0;
  out[0] = 0;
  for (int i = 0; i < n; i++) {
    at += tokdec_append(t, ids[i], out, out_size, at);
  }

  // Drop leading space, and make bytes valid UTF-8
  size_t r = 0, w = 0;
  if (at > 0 && out[0] == ' ') {
    r = 1;
  }

  while (r < at) {
    int k = utf8_len((const unsigned char *)out + r, at - r);
    if (k == 0) {
      out[w++] = '?';
      r++;
    } else {
      for (int j = 0; j < k; j++) {
        out[w++] = out[r + (size_t)j];
      }

      r += (size_t)k;
    }
  }

  out[w] = 0;
  return w;
}

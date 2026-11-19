/*
 * tests/asrnet_fixture.h - small random model of asrnet tests
 *
 * 32 wide, 3 + 2 layers, weights from a PRNG that tests/gen_asrnet_vectors.py
 * has too, written to a safetensors buffer
 * Needs test's own pass/fail counters (CHECK) and asrnet_vectors.inc included
 * before it
 */

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "../lib/nolibc.h"
#include "../lib/nl_math.h"
#include "asrnet.h"

static unsigned prng_s;
static double prng(void) {
  unsigned s = prng_s;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  prng_s = s;

  return (double)s / 4294967296.0;
}

typedef struct {
  char name[96];
  int nd;
  size_t shape[3];
  double scale;
  char kind; // w weight, g gamma, o around 1, l log_k
} tdef_t;

static tdef_t defs[200];
static int ndefs;

static void def(const char *name, double scale, char kind, int nd, size_t a,
                size_t b, size_t c) {
  tdef_t *t = &defs[ndefs++];
  nl_strncpy_safe(t->name, name, sizeof(t->name));
  t->nd = nd;
  t->shape[0] = a;
  t->shape[1] = b;
  t->shape[2] = c;
  t->scale = scale;
  t->kind = kind;
}

static an_config_t cfg;

static void make_defs(void) {
  ndefs = 0;
  size_t h = 32, f = 64, d = 32, df = 48, V = 50, P = 64;
  char n[96];
  def("model.encoder.embedder.comp.log_k", 0, 'l', 1, 1, 0, 0);
  def("model.encoder.embedder.linear.weight", 0.25, 'w', 2, h, 80, 0);
  def("model.encoder.embedder.conv1.weight", 0.12, 'w', 3, 2 * h, h, 5);
  def("model.encoder.embedder.conv1.bias", 0.1, 'w', 1, 2 * h, 0, 0);
  def("model.encoder.embedder.conv2.weight", 0.08, 'w', 3, h, 2 * h, 5);
  def("model.encoder.embedder.conv2.bias", 0.1, 'w', 1, h, 0, 0);
  for (int l = 0; l < 3; l++) {
    for (int q = 0; q < 4; q++) {
      nl_snprintf(n, sizeof(n),
                  "model.encoder.layers.%d.self_attn.%c_proj.weight", l,
                  "qkvo"[q]);
      def(n, 0.3, 'w', 2, h, h, 0);
    }

    nl_snprintf(n, sizeof(n), "model.encoder.layers.%d.input_layernorm.gamma",
                l);
    def(n, 0.3, 'g', 1, h, 0, 0);
    nl_snprintf(n, sizeof(n),
                "model.encoder.layers.%d.post_attention_layernorm.gamma", l);
    def(n, 0.3, 'g', 1, h, 0, 0);
    nl_snprintf(n, sizeof(n), "model.encoder.layers.%d.mlp.fc1.weight", l);
    def(n, 0.3, 'w', 2, f, h, 0);
    nl_snprintf(n, sizeof(n), "model.encoder.layers.%d.mlp.fc1.bias", l);
    def(n, 0.1, 'w', 1, f, 0, 0);
    nl_snprintf(n, sizeof(n), "model.encoder.layers.%d.mlp.fc2.weight", l);
    def(n, 0.25, 'w', 2, h, f, 0);
    nl_snprintf(n, sizeof(n), "model.encoder.layers.%d.mlp.fc2.bias", l);
    def(n, 0.1, 'w', 1, h, 0, 0);
  }

  def("model.encoder.final_norm.gamma", 0.3, 'g', 1, h, 0, 0);
  def("model.decoder.embed_tokens.weight", 0.6, 'w', 2, V, d, 0);
  def("model.decoder.pos_emb.weight", 0.3, 'w', 2, P, h, 0);
  for (int l = 0; l < 2; l++) {
    for (int b = 0; b < 2; b++) {
      for (int q = 0; q < 4; q++) {
        nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.%s.%c_proj.weight",
                    l, b ? "encoder_attn" : "self_attn", "qkvo"[q]);
        def(n, 0.3, 'w', 2, d, d, 0);
      }
    }

    const char *ln3[3] = {"input_layernorm", "post_attention_layernorm",
                          "final_layernorm"};
    for (int i = 0; i < 3; i++) {
      nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.%s.weight", l, ln3[i]);
      def(n, 0.3, 'o', 1, d, 0, 0);
    }

    nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.mlp.fc1.weight", l);
    def(n, 0.3, 'w', 2, 2 * df, d, 0);
    nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.mlp.fc1.bias", l);
    def(n, 0.1, 'w', 1, 2 * df, 0, 0);
    nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.mlp.fc2.weight", l);
    def(n, 0.25, 'w', 2, d, df, 0);
    nl_snprintf(n, sizeof(n), "model.decoder.layers.%d.mlp.fc2.bias", l);
    def(n, 0.1, 'w', 1, d, 0, 0);
  }

  def("model.decoder.norm.weight", 0.3, 'o', 1, d, 0, 0);
  def("proj_out.weight", 0.5, 'w', 2, V, d, 0);
}

static size_t numel(const tdef_t *t) {
  size_t n = 1;
  for (int i = 0; i < t->nd; i++) {
    n *= t->shape[i];
  }

  return n;
}

// File: dtype 0 = F32, 1 = F16 (only for loader test), 2 = BF16
static unsigned char *build_file(size_t *size, int skip, int wrong_shape,
                                 int dtype) {
  size_t total = 0;
  for (int i = 0; i < ndefs; i++) {
    total += numel(&defs[i]) * 4;
  }

  char *hdr = nl_malloc(ndefs * 300 + 64);
  size_t hn = 0;
  hdr[hn++] = '{';
  size_t off = 0;
  int first = 1;
  for (int i = 0; i < ndefs; i++) {
    if (i == skip) {
      continue;
    }

    size_t bytes = numel(&defs[i]) * (dtype ? 2 : 4);
    char shape[64];
    size_t sn = 0;
    if (i == wrong_shape) { // same number of values, other shape
      sn = (size_t)nl_snprintf(shape, sizeof(shape), "1,%d",
                               (int)numel(&defs[i]));
    }

    for (int k = 0; i != wrong_shape && k < defs[i].nd; k++) {
      sn += (size_t)nl_snprintf(shape + sn, sizeof(shape) - sn, "%s%d",
                                k ? "," : "", (int)defs[i].shape[k]);
    }

    hn += (size_t)nl_snprintf(
        hdr + hn, 300,
        "%s\"%s\":{\"dtype\":\"%s\",\"shape\":[%s],\"data_offsets\":[%d,%d]}",
        first ? "" : ",", defs[i].name,
        dtype == 1   ? "F16"
        : dtype == 2 ? "BF16"
                     : "F32",
        shape, (int)off, (int)(off + bytes));
    first = 0;
    off += bytes;
  }

  hdr[hn++] = '}';
  while (hn % 8) {
    hdr[hn++] = ' ';
  }

  unsigned char *file = nl_malloc(8 + hn + total);
  for (int i = 0; i < 8; i++) {
    file[i] = (unsigned char)((hn >> (8 * i)) & 255);
  }

  nl_memcpy(file + 8, hdr, hn);
  nl_free(hdr);
  unsigned char *p = file + 8 + hn;
  // Exact order of operations of Python generator
  prng_s = 0x1234ABCDu;
  for (int i = 0; i < ndefs; i++) {
    if (i == skip) {
      // Stream of random numbers still advances
    }

    size_t n = numel(&defs[i]);
    for (size_t k = 0; k < n; k++) {
      float v;
      if (defs[i].kind == 'l') {
        v = (float)nl_log(0.75);
      } else {
        double x = (prng() - 0.5) * defs[i].scale;
        if (defs[i].kind == 'o') {
          x += 1.0;
        }

        v = (float)x;
      }

      if (i == skip) {
        continue;
      }

      if (dtype == 0) {
        nl_memcpy(p, &v, 4);
        p += 4;
      } else if (dtype == 2) { // bf16: the top half of the float
        unsigned u;
        nl_memcpy(&u, &v, 4);
        unsigned short h = (unsigned short)(u >> 16);
        nl_memcpy(p, &h, 2);
        p += 2;
      } else { // f16: only used for values that are exact, see the test
        unsigned short h = 0;
        nl_memcpy(p, &h, 2);
        p += 2;
      }
    }
  }

  *size = (size_t)(p - file);

  return file;
}

static int16_t audio_s16[MV_AUDIO];
static float audio_f[MV_AUDIO];

static void make_audio(void) {
  prng_s = 777;
  for (int i = 0; i < MV_AUDIO; i++) {
    double env = 0.05 + 0.9 * (double)((i / 700) % 5) / 4.0;
    audio_f[i] = (float)((prng() - 0.5) * 2.0 * env);
  }
}

static void make_cfg(void) {
  an_config_tiny(&cfg);
  cfg.hid = 32;
  cfg.ffn = 64;
  cfg.layers = 3;
  cfg.heads = 2;
  cfg.dhid = 32;
  cfg.dffn = 48;
  cfg.dlayers = 2;
  cfg.dheads = 2;
  cfg.vocab = 50;
  int wl[3] = {5, 5, 4}, wr[3] = {4, 0, 3};
  for (int i = 0; i < 3; i++) {
    cfg.win_l[i] = wl[i];
    cfg.win_r[i] = wr[i];
  }
}

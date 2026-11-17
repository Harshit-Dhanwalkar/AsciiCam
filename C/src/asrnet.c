/*
 * src/asrnet.c - streaming speech recognition network
 *
 * Network is written out operation by operation
 */

#include "platform.h"

#include "nolibc.h"

#include "asrnet.h"
#include "nl_math.h"
#include "nn.h"

#include <stdint.h>

#ifdef __LINUX_NOLIBC__
void *memmove(void *dest, const void *src, size_t n);
#endif

struct an_stream {
  const an_model_t *m;
  const an_config_t *c;

  // Frontend
  float frame[AN_FRAME];
  int nframe;
  long n_h0; // 5 ms frames done
  long n_c1; // conv1 outputs done
  long n_c2; // conv2 outputs (layer 0 inputs) done
  float *h0; // last 5 frontend frames, oldest first: 5 * hid
  float *c1; // last 5 conv1 outputs after silu: 5 * 2hid
  int flushed;

  // Encoder layers
  struct {
    float *x, *k, *v; // AN_RING rows of hid
    long nin, nout;
  } L[AN_MAX_LAYERS];

  // Encoder output and decoder's view of it
  float *enc; // cap * hid
  long nenc;
  long cap;
  float *ck[AN_MAX_LAYERS]; // cap * dhid
  float *cv[AN_MAX_LAYERS];

  // Scratch
  float *t1, *t2, *t3, *t4, *t5; // ffn sized
  float *scores;
};

void an_config_tiny(an_config_t *c) {
  nl_memset(c, 0, sizeof(*c));
  c->hid = 320;
  c->ffn = 1280;
  c->layers = 6;
  c->heads = 8;
  c->dhid = 320;
  c->dffn = 1280;
  c->dlayers = 6;
  c->dheads = 8;
  c->vocab = 32768;
  static const int wl[6] = {16, 16, 16, 16, 16, 16};
  static const int wr[6] = {4, 4, 0, 0, 4, 4};
  for (int i = 0; i < 6; i++) {
    c->win_l[i] = wl[i];
    c->win_r[i] = wr[i];
  }

  c->rope_theta = 10000.0f;
  c->rope_partial = 0.8f;
  c->start_tok = 1;
  c->eos_tok = 2;
}

/* Loading */

static void fail(char *err, size_t size, const char *what, const char *name) {
  if (err && size) {
    nl_snprintf(err, size, "%s: %s", what, name ? name : "");
  }
}

static const float *own(an_model_t *m, float *p) {
  if (p && m->nowned < AN_OWNED) {
    m->owned[m->nowned++] = p;
  }

  return p;
}

// Look up a tensor under either "model.<name>" or "<name>", check its shape
static const float *want(an_model_t *m, const char *name, size_t d0, size_t d1,
                         size_t d2, char *err, size_t err_size, int *bad) {
  char full[ST_NAME];
  const st_tensor_t *t = NULL;
  nl_snprintf(full, sizeof(full), "model.%s", name);
  t = st_find(&m->st, full);
  if (!t) {
    t = st_find(&m->st, name);
  }

  if (!t) {
    if (!*bad) {
      fail(err, err_size, "missing tensor", name);
    }

    *bad = 1;

    return NULL;
  }

  size_t want_n = d0 * (d1 ? d1 : 1) * (d2 ? d2 : 1);
  int dims = d2 ? 3 : d1 ? 2 : 1;
  int ok = t->numel == want_n && t->ndim == dims;
  if (ok) {
    size_t w[3] = {d0, d1, d2};
    for (int i = 0; i < dims; i++) {
      ok = ok && t->shape[i] == w[i];
    }
  }

  if (!ok) {
    if (!*bad) {
      fail(err, err_size, "tensor has the wrong shape", name);
    }

    *bad = 1;

    return NULL;
  }

  const float *f = st_f32(&m->st, t);
  if (!f && !*bad) {
    fail(err, err_size, "unsupported tensor type (use F32, F16 or BF16)", name);
    *bad = 1;
  }

  return f;
}

// gamma + 1 (encoder's layer norm has a unit offset)
static const float *want_gamma(an_model_t *m, const char *name, size_t n,
                               char *err, size_t err_size, int *bad) {
  const float *g = want(m, name, n, 0, 0, err, err_size, bad);
  if (!g) {
    return NULL;
  }

  float *c = nl_malloc(n * sizeof(float));
  if (!c) {
    *bad = 1;

    return NULL;
  }

  for (size_t i = 0; i < n; i++) {
    c[i] = g[i] + 1.0f;
  }

  return own(m, c);
}

static float *repack_conv(an_model_t *m, const float *w, size_t cout,
                          size_t cin, size_t k) {
  float *r = nl_malloc(cout * cin * k * sizeof(float));
  if (!r) {
    return NULL;
  }

  for (size_t o = 0; o < cout; o++) {
    for (size_t c = 0; c < cin; c++) {
      for (size_t j = 0; j < k; j++) {
        r[o * k * cin + j * cin + c] = w[(o * cin + c) * k + j];
      }
    }
  }

  own(m, r);

  return r;
}

static int load_attn(an_model_t *m, an_attn_t *a, const char *prefix,
                     size_t hid_in, size_t hid_out, char *err, size_t es,
                     int *bad) {
  char n[ST_NAME];
  nl_snprintf(n, sizeof(n), "%s.q_proj.weight", prefix);
  a->q = want(m, n, hid_out, hid_in, 0, err, es, bad);
  nl_snprintf(n, sizeof(n), "%s.k_proj.weight", prefix);
  a->k = want(m, n, hid_out, hid_in, 0, err, es, bad);
  nl_snprintf(n, sizeof(n), "%s.v_proj.weight", prefix);
  a->v = want(m, n, hid_out, hid_in, 0, err, es, bad);
  nl_snprintf(n, sizeof(n), "%s.o_proj.weight", prefix);
  a->o = want(m, n, hid_in, hid_out, 0, err, es, bad);

  return *bad;
}

int an_load_st(an_model_t *m, const st_t *st, const an_config_t *cfg, char *err,
               size_t err_size) {
  nl_memset(m, 0, sizeof(*m));
  m->st = *st;
  if (cfg) {
    m->cfg = *cfg;
  } else {
    an_config_tiny(&m->cfg);
  }

  const an_config_t *c = &m->cfg;
  int bad = 0;
  if (c->layers < 1 || c->layers > AN_MAX_LAYERS || c->dlayers < 1 ||
      c->dlayers > AN_MAX_LAYERS || c->heads < 1 || c->dheads < 1 ||
      c->hid % c->heads || c->dhid % c->dheads) {
    fail(err, err_size, "bad model configuration", NULL);
    nl_memset(m, 0, sizeof(*m));

    return -1;
  }

  size_t hid = (size_t)c->hid, ffn = (size_t)c->ffn;
  size_t dhid = (size_t)c->dhid, dffn = (size_t)c->dffn;
  size_t dhd = (size_t)(c->dhid / c->dheads);
  size_t rot = (size_t)((float)dhd * c->rope_partial);
  if (rot % 2 || rot == 0 || rot > dhd) {
    fail(err, err_size, "bad rotary dimension", NULL);
    nl_memset(m, 0, sizeof(*m));

    return -1;
  }

  const float *lk =
      want(m, "encoder.embedder.comp.log_k", 1, 0, 0, err, err_size, &bad);
  m->log_k = lk ? lk[0] : 0.0f;
  m->lin_w = want(m, "encoder.embedder.linear.weight", hid, AN_FRAME, 0, err,
                  err_size, &bad);
  const float *c1w = want(m, "encoder.embedder.conv1.weight", 2 * hid, hid, 5,
                          err, err_size, &bad);
  m->conv1_b = want(m, "encoder.embedder.conv1.bias", 2 * hid, 0, 0, err,
                    err_size, &bad);
  const float *c2w = want(m, "encoder.embedder.conv2.weight", hid, 2 * hid, 5,
                          err, err_size, &bad);
  m->conv2_b =
      want(m, "encoder.embedder.conv2.bias", hid, 0, 0, err, err_size, &bad);
  if (c1w && c2w) {
    m->conv1_w = repack_conv(m, c1w, 2 * hid, hid, 5);
    m->conv2_w = repack_conv(m, c2w, hid, 2 * hid, 5);
    if (!m->conv1_w || !m->conv2_w) {
      bad = 1;
    }
  }

  for (int l = 0; l < c->layers && !bad; l++) {
    char p[ST_NAME], n[ST_NAME];
    an_enc_layer_t *e = &m->enc[l];
    nl_snprintf(p, sizeof(p), "encoder.layers.%d.self_attn", l);
    load_attn(m, &e->attn, p, hid, hid, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "encoder.layers.%d.input_layernorm.gamma", l);
    e->ln1 = want_gamma(m, n, hid, err, err_size, &bad);
    nl_snprintf(n, sizeof(n),
                "encoder.layers.%d.post_attention_layernorm.gamma", l);
    e->ln2 = want_gamma(m, n, hid, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "encoder.layers.%d.mlp.fc1.weight", l);
    e->fc1w = want(m, n, ffn, hid, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "encoder.layers.%d.mlp.fc1.bias", l);
    e->fc1b = want(m, n, ffn, 0, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "encoder.layers.%d.mlp.fc2.weight", l);
    e->fc2w = want(m, n, hid, ffn, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "encoder.layers.%d.mlp.fc2.bias", l);
    e->fc2b = want(m, n, hid, 0, 0, err, err_size, &bad);
  }

  m->enc_norm =
      want_gamma(m, "encoder.final_norm.gamma", hid, err, err_size, &bad);
  m->embed = want(m, "decoder.embed_tokens.weight", (size_t)c->vocab, dhid, 0,
                  err, err_size, &bad);

  // Position table is added to encoder output; its length is whatever file has,
  // at least as long as a stream can get
  char pn[ST_NAME];
  nl_snprintf(pn, sizeof(pn), "model.decoder.pos_emb.weight");
  const st_tensor_t *pt = st_find(&m->st, pn);
  if (!pt) {
    pt = st_find(&m->st, "decoder.pos_emb.weight");
  }

  if (!pt || pt->ndim != 2 || pt->shape[1] != hid) {
    if (!bad) {
      fail(err, err_size, "missing or misshapen tensor",
           "decoder.pos_emb.weight");
    }

    bad = 1;
  } else {
    m->pos_rows = pt->shape[0];
    m->pos_emb = st_f32(&m->st, pt);
    if (!m->pos_emb) {
      bad = 1;
    }
  }

  if (!bad && hid != dhid) {
    m->proj = want(m, "decoder.proj.weight", dhid, hid, 0, err, err_size, &bad);
  }

  for (int l = 0; l < c->dlayers && !bad; l++) {
    char p[ST_NAME], n[ST_NAME];
    an_dec_layer_t *d = &m->dec[l];
    nl_snprintf(p, sizeof(p), "decoder.layers.%d.self_attn", l);
    load_attn(m, &d->self, p, dhid, dhid, err, err_size, &bad);
    nl_snprintf(p, sizeof(p), "decoder.layers.%d.encoder_attn", l);
    load_attn(m, &d->cross, p, dhid, dhid, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.input_layernorm.weight", l);
    d->ln_in = want(m, n, dhid, 0, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n),
                "decoder.layers.%d.post_attention_layernorm.weight", l);
    d->ln_post = want(m, n, dhid, 0, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.final_layernorm.weight", l);
    d->ln_final = want(m, n, dhid, 0, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.mlp.fc1.weight", l);
    d->fc1w = want(m, n, 2 * dffn, dhid, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.mlp.fc1.bias", l);
    d->fc1b = want(m, n, 2 * dffn, 0, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.mlp.fc2.weight", l);
    d->fc2w = want(m, n, dhid, dffn, 0, err, err_size, &bad);
    nl_snprintf(n, sizeof(n), "decoder.layers.%d.mlp.fc2.bias", l);
    d->fc2b = want(m, n, dhid, 0, 0, err, err_size, &bad);
  }

  m->dec_norm = want(m, "decoder.norm.weight", dhid, 0, 0, err, err_size, &bad);
  // proj_out is separate (tie_word_embeddings false); if it is absent embedding
  // table is output matrix
  const st_tensor_t *po = st_find(&m->st, "proj_out.weight");
  if (!po) {
    po = st_find(&m->st, "model.proj_out.weight");
  }

  if (po) {
    m->proj_out =
        want(m, po->name, (size_t)c->vocab, dhid, 0, err, err_size, &bad);
  } else {
    m->proj_out = m->embed;
  }

  if (bad) {
    for (int i = 0; i < m->nowned; i++) {
      nl_free(m->owned[i]);
    }

    nl_memset(m, 0, sizeof(*m));

    return -1;
  }

  return 0;
}

int an_load(an_model_t *m, const char *path, const an_config_t *cfg, char *err,
            size_t err_size) {
  st_t st;
  if (st_open(&st, path, err, err_size) != 0) {
    return -1;
  }

  if (an_load_st(m, &st, cfg, err, err_size) != 0) {
    st_close(&st);

    return -1;
  }

  return 0;
}

void an_free(an_model_t *m) {
  for (int i = 0; i < m->nowned; i++) {
    nl_free(m->owned[i]);
  }

  st_close(&m->st);
  nl_memset(m, 0, sizeof(*m));
}

/* Streams */

static void zero_state(an_stream_t *s) {
  const an_config_t *c = s->c;
  s->nframe = 0;
  s->n_h0 = s->n_c1 = s->n_c2 = 0;
  s->flushed = 0;
  s->nenc = 0;
  nl_memset(s->h0, 0, 5 * (size_t)c->hid * sizeof(float));
  nl_memset(s->c1, 0, 5 * 2 * (size_t)c->hid * sizeof(float));
  for (int l = 0; l < c->layers; l++) {
    s->L[l].nin = s->L[l].nout = 0;
  }
}

an_stream_t *an_stream_new(const an_model_t *m) {
  return an_stream_new_cap(m, AN_MAX_ENC);
}

an_stream_t *an_stream_new_cap(const an_model_t *m, long cap) {
  if (cap < 1 || cap > AN_MAX_ENC) {
    return NULL;
  }

  const an_config_t *c = &m->cfg;
  an_stream_t *s = nl_calloc(1, sizeof(*s));
  if (!s) {
    return NULL;
  }

  s->m = m;
  s->c = &m->cfg;
  s->cap = cap;
  size_t hid = (size_t)c->hid;
  size_t dhid = (size_t)c->dhid;
  size_t big = (size_t)c->ffn > (size_t)c->dffn * 2 ? (size_t)c->ffn
                                                    : (size_t)c->dffn * 2;
  int ok = 1;
  s->h0 = nl_calloc(5 * hid, sizeof(float));
  s->c1 = nl_calloc(5 * 2 * hid, sizeof(float));
  s->enc = nl_calloc((size_t)cap * hid, sizeof(float));
  s->t1 = nl_calloc(big + 4 * hid + 4 * dhid, sizeof(float));
  s->t2 = nl_calloc(big + 4 * hid + 4 * dhid, sizeof(float));
  s->t3 = nl_calloc(big + 4 * hid + 4 * dhid, sizeof(float));
  s->t4 = nl_calloc(big + 4 * hid + 4 * dhid, sizeof(float));
  s->t5 = nl_calloc(big + 4 * hid + 4 * dhid, sizeof(float));
  s->scores = nl_calloc((size_t)cap + 8, sizeof(float));
  ok = s->h0 && s->c1 && s->enc && s->t1 && s->t2 && s->t3 && s->t4 && s->t5 &&
       s->scores;
  for (int l = 0; ok && l < c->layers; l++) {
    s->L[l].x = nl_calloc(AN_RING * hid, sizeof(float));
    s->L[l].k = nl_calloc(AN_RING * hid, sizeof(float));
    s->L[l].v = nl_calloc(AN_RING * hid, sizeof(float));
    ok = s->L[l].x && s->L[l].k && s->L[l].v;
  }

  for (int l = 0; ok && l < c->dlayers; l++) {
    s->ck[l] = nl_calloc((size_t)cap * dhid, sizeof(float));
    s->cv[l] = nl_calloc((size_t)cap * dhid, sizeof(float));
    ok = s->ck[l] && s->cv[l];
  }

  if (!ok) {
    an_stream_free(s);

    return NULL;
  }

  zero_state(s);

  return s;
}

void an_stream_free(an_stream_t *s) {
  if (!s) {
    return;
  }

  nl_free(s->h0);
  nl_free(s->c1);
  nl_free(s->enc);
  nl_free(s->t1);
  nl_free(s->t2);
  nl_free(s->t3);
  nl_free(s->t4);
  nl_free(s->t5);
  nl_free(s->scores);
  for (int l = 0; l < AN_MAX_LAYERS; l++) {
    nl_free(s->L[l].x);
    nl_free(s->L[l].k);
    nl_free(s->L[l].v);
    nl_free(s->ck[l]);
    nl_free(s->cv[l]);
  }

  nl_free(s);
}

void an_stream_reset(an_stream_t *s) { zero_state(s); }

long an_frames(const an_stream_t *s) { return s->nenc; }

int an_full(const an_stream_t *s) { return s->nenc >= s->cap; }

const float *an_encoder_frame(const an_stream_t *s, long i) {
  if (i < 0 || i >= s->nenc) {
    return NULL;
  }

  return s->enc + (size_t)i * (size_t)s->c->hid;
}

/* Encoder */

static int future_of(const an_config_t *c, int l) {
  return c->win_r[l] > 0 ? c->win_r[l] - 1 : 0;
}

// Last stage: a finished encoder frame goes out, and decoder's cross attention
// keys and values for it are made
static void enc_out(an_stream_t *s, const float *y) {
  const an_model_t *m = s->m;
  const an_config_t *c = s->c;
  size_t hid = (size_t)c->hid, dhid = (size_t)c->dhid;
  if (s->nenc >= s->cap) {
    return;
  }

  long t = s->nenc;
  float *e = s->enc + (size_t)t * hid;
  nn_layernorm(e, y, 1, hid, 1e-5f, m->enc_norm, NULL);
  s->nenc++;

  // encoder_hidden_states += pos_emb[t], then proj
  if ((size_t)t >= m->pos_rows) {
    return;
  }

  float *p = s->t1;
  for (size_t i = 0; i < hid; i++) {
    p[i] = e[i] + m->pos_emb[(size_t)t * hid + i];
  }

  const float *src = p;
  if (m->proj) {
    nn_linear(s->t2, p, 1, m->proj, NULL, hid, dhid);
    src = s->t2;
  }

  for (int l = 0; l < c->dlayers; l++) {
    nn_linear(s->ck[l] + (size_t)t * dhid, src, 1, m->dec[l].cross.k, NULL,
              dhid, dhid);
    nn_linear(s->cv[l] + (size_t)t * dhid, src, 1, m->dec[l].cross.v, NULL,
              dhid, dhid);
  }
}

static void layer_run(an_stream_t *s, int l, int flush);

// Frame x enters layer l
static void layer_push(an_stream_t *s, int l, const float *x) {
  const an_config_t *c = s->c;
  const an_enc_layer_t *e = &s->m->enc[l];
  size_t hid = (size_t)c->hid;
  long t = s->L[l].nin;
  size_t row = (size_t)t % AN_RING;
  nl_memcpy(s->L[l].x + row * hid, x, hid * sizeof(float));
  float *h = s->t3;
  nn_layernorm(h, x, 1, hid, 1e-5f, e->ln1, NULL);
  nn_linear(s->L[l].k + row * hid, h, 1, e->attn.k, NULL, hid, hid);
  nn_linear(s->L[l].v + row * hid, h, 1, e->attn.v, NULL, hid, hid);
  s->L[l].nin++;
  layer_run(s, l, 0);
}

// Emit what can be emitted: frame t once t + lookahead has arrived (or
// everything when flushing)
static void layer_run(an_stream_t *s, int l, int flush) {
  const an_config_t *c = s->c;
  const an_enc_layer_t *e = &s->m->enc[l];
  size_t hid = (size_t)c->hid, ffn = (size_t)c->ffn;
  size_t hd = hid / (size_t)c->heads;
  int fut = future_of(c, l);

  while (s->L[l].nout < s->L[l].nin &&
         (flush || s->L[l].nout + fut < s->L[l].nin)) {
    long t = s->L[l].nout;
    size_t row = (size_t)t % AN_RING;
    const float *x = s->L[l].x + row * hid;
    float *h = s->t3, *q = s->t4, *y = s->t5;

    nn_layernorm(h, x, 1, hid, 1e-5f, e->ln1, NULL);
    nn_linear(q, h, 1, e->attn.q, NULL, hid, hid);

    long first = t - (c->win_l[l] - 1);
    if (first < 0) {
      first = 0;
    }

    long last = t + fut;
    if (last > s->L[l].nin - 1) {
      last = s->L[l].nin - 1;
    }

    float *att = s->t1;
    nn_attend(att, q, s->L[l].k, s->L[l].v, hid, AN_RING, (size_t)first,
              (size_t)(last - first + 1), (size_t)c->heads, hd, s->scores);
    nn_linear(y, att, 1, e->attn.o, NULL, hid, hid);
    for (size_t i = 0; i < hid; i++) {
      y[i] += x[i];
    }

    // Feed forward on residual stream
    nn_layernorm(h, y, 1, hid, 1e-5f, e->ln2, NULL);
    float *f1 = s->t1;
    nn_linear(f1, h, 1, e->fc1w, e->fc1b, hid, ffn);
    nn_gelu(f1, ffn);
    nn_linear(q, f1, 1, e->fc2w, e->fc2b, ffn, hid);
    for (size_t i = 0; i < hid; i++) {
      y[i] += q[i];
    }

    s->L[l].nout++;
    // y is in t5, which next layer's push does not touch
    float out[1024];
    if (hid <= 1024) {
      nl_memcpy(out, y, hid * sizeof(float));
      if (l + 1 < c->layers) {
        layer_push(s, l + 1, out);
      } else {
        enc_out(s, out);
      }
    }
  }
}

// One finished conv2 output enters layers
static void conv2_out(an_stream_t *s, const float *y) { layer_push(s, 0, y); }

// conv1 output j is ready: it joins history, and every second one makes a conv2
// output
static void conv1_out(an_stream_t *s, const float *c1v) {
  const an_model_t *m = s->m;
  size_t hid = (size_t)s->c->hid, h2 = 2 * hid;
  // history shifts by one frame
  memmove(s->c1, s->c1 + h2, 4 * h2 * sizeof(float));
  nl_memcpy(s->c1 + 4 * h2, c1v, h2 * sizeof(float));
  long j = s->n_c1++;
  if (j % 2 == 0) {
    float y[1024];
    if (hid > 1024) {
      return;
    }

    // Causal conv, kernel 5 over 5 frames of history
    nn_linear(y, s->c1, 1, m->conv2_w, m->conv2_b, 5 * h2, hid);
    conv2_out(s, y);
    s->n_c2++;
  }
}

static void frame_in(an_stream_t *s, const float *x) {
  const an_model_t *m = s->m;
  size_t hid = (size_t)s->c->hid;
  // Streaming Frame CMVN
  double mean = 0;
  for (int i = 0; i < AN_FRAME; i++) {
    mean += x[i];
  }

  mean /= AN_FRAME;
  double var = 0;
  float cx[AN_FRAME];
  for (int i = 0; i < AN_FRAME; i++) {
    double d = x[i] - mean;
    cx[i] = (float)d;
    var += d * d;
  }

  var /= AN_FRAME;
  float inv = (float)(1.0 / nl_sqrt(var + 1e-6));
  // Streaming A sinh Compression
  float k = (float)nl_exp((double)m->log_k);
  for (int i = 0; i < AN_FRAME; i++) {
    cx[i] = (float)nl_asinh((double)(k * cx[i] * inv));
  }

  // SiLu(linear(frame))
  float h[1024];
  if (hid > 1024) {
    return;
  }

  nn_linear(h, cx, 1, m->lin_w, NULL, AN_FRAME, hid);
  nn_silu(h, hid);
  memmove(s->h0, s->h0 + hid, 4 * hid * sizeof(float));
  nl_memcpy(s->h0 + 4 * hid, h, hid * sizeof(float));
  long f = s->n_h0++;
  if (f % 2 == 0) {
    float y[2048];
    if (2 * hid > 2048) {
      return;
    }

    nn_linear(y, s->h0, 1, m->conv1_w, m->conv1_b, 5 * hid, 2 * hid);
    nn_silu(y, 2 * hid);
    conv1_out(s, y);
  }
}

void an_feed(an_stream_t *s, const float *pcm, size_t n) {
  if (s->flushed) {
    return;
  }

  for (size_t i = 0; i < n; i++) {
    s->frame[s->nframe++] = pcm[i];
    if (s->nframe == AN_FRAME) {
      frame_in(s, s->frame);
      s->nframe = 0;
    }
  }
}

void an_feed_s16(an_stream_t *s, const int16_t *pcm, size_t n) {
  float buf[256];
  size_t i = 0;
  while (i < n) {
    size_t k = n - i < 256 ? n - i : 256;
    for (size_t j = 0; j < k; j++) {
      buf[j] = (float)pcm[i + j] * (1.0f / 32768.0f);
    }

    an_feed(s, buf, k);
    i += k;
  }
}

void an_flush(an_stream_t *s) {
  if (s->flushed) {
    return;
  }

  s->flushed = 1;
  for (int l = 0; l < s->c->layers; l++) {
    layer_run(s, l, 1);
  }
}

/* Decoder */

// Runs decoder over prefix[0..np) (teacher forcing), then, if max_new > 0,
// carries on with greedy decoding
// Writes logits after prefix to logits_last if given
// Returns number of generated tokens, -1 on error
static int dec_run(an_stream_t *s, const int *prefix, int np, int *out,
                   int max_new, float *logits_last) {
  const an_model_t *m = s->m;
  const an_config_t *c = s->c;
  size_t dhid = (size_t)c->dhid, dffn = (size_t)c->dffn;
  size_t dhd = dhid / (size_t)c->dheads;
  size_t rot = (size_t)((float)dhd * c->rope_partial);
  size_t total = (size_t)np + (size_t)(max_new > 0 ? max_new : 0);
  if (np < 1 || s->nenc == 0 || dhid > 1024) {
    return -1;
  }

  float *sk[AN_MAX_LAYERS] = {0}, *sv[AN_MAX_LAYERS] = {0};
  float *logits = nl_malloc((size_t)c->vocab * sizeof(float));
  float *scores = nl_malloc((total + (size_t)s->nenc + 8) * sizeof(float));
  int ok = logits && scores;
  for (int l = 0; ok && l < c->dlayers; l++) {
    sk[l] = nl_malloc(total * dhid * sizeof(float));
    sv[l] = nl_malloc(total * dhid * sizeof(float));
    ok = sk[l] && sv[l];
  }

  int ng = 0;
  if (ok) {
    float x[1024], h[1024], q[1024], k[1024], v[1024], a[1024];
    float *ff = s->t1; // 2 * dffn floats
    int tok = prefix[0];
    for (size_t pos = 0; pos < total; pos++) {
      if (pos < (size_t)np) {
        tok = prefix[pos];
      }

      if (tok < 0 || tok >= c->vocab) {
        ng = -1;
        break;
      }

      nl_memcpy(x, m->embed + (size_t)tok * dhid, dhid * sizeof(float));
      for (int l = 0; l < c->dlayers; l++) {
        const an_dec_layer_t *d = &m->dec[l];
        // self attention, causal, with rotary embedding
        nn_layernorm(h, x, 1, dhid, 1e-5f, d->ln_in, NULL);
        nn_linear(q, h, 1, d->self.q, NULL, dhid, dhid);
        nn_linear(k, h, 1, d->self.k, NULL, dhid, dhid);
        nn_linear(v, h, 1, d->self.v, NULL, dhid, dhid);
        nn_rope(q, (size_t)c->dheads, dhd, rot, pos, c->rope_theta);
        nn_rope(k, (size_t)c->dheads, dhd, rot, pos, c->rope_theta);
        nl_memcpy(sk[l] + pos * dhid, k, dhid * sizeof(float));
        nl_memcpy(sv[l] + pos * dhid, v, dhid * sizeof(float));
        nn_attend(a, q, sk[l], sv[l], dhid, 0, 0, pos + 1, (size_t)c->dheads,
                  dhd, scores);
        nn_linear(h, a, 1, d->self.o, NULL, dhid, dhid);
        for (size_t i = 0; i < dhid; i++) {
          x[i] += h[i];
        }

        // Cross attention to encoder, no mask: every frame is real
        nn_layernorm(h, x, 1, dhid, 1e-5f, d->ln_post, NULL);
        nn_linear(q, h, 1, d->cross.q, NULL, dhid, dhid);
        nn_attend(a, q, s->ck[l], s->cv[l], dhid, 0, 0, (size_t)s->nenc,
                  (size_t)c->dheads, dhd, scores);
        nn_linear(h, a, 1, d->cross.o, NULL, dhid, dhid);
        for (size_t i = 0; i < dhid; i++) {
          x[i] += h[i];
        }

        // Gated feed forward: fc1 gives [value | gate], silu on gate
        nn_layernorm(h, x, 1, dhid, 1e-5f, d->ln_final, NULL);
        nn_linear(ff, h, 1, d->fc1w, d->fc1b, dhid, 2 * dffn);
        float *gate = ff + dffn;
        nn_silu(gate, dffn);
        for (size_t i = 0; i < dffn; i++) {
          ff[i] *= gate[i];
        }

        nn_linear(h, ff, 1, d->fc2w, d->fc2b, dffn, dhid);
        for (size_t i = 0; i < dhid; i++) {
          x[i] += h[i];
        }
      }

      if (pos + 1 < (size_t)np) {
        continue; // Logits are only needed from last prefix token on
      }

      nn_layernorm(h, x, 1, dhid, 1e-5f, m->dec_norm, NULL);
      nn_linear(logits, h, 1, m->proj_out, NULL, dhid, (size_t)c->vocab);
      if (pos + 1 == (size_t)np && logits_last) {
        nl_memcpy(logits_last, logits, (size_t)c->vocab * sizeof(float));
      }

      if (max_new <= 0) {
        break;
      }

      int best = 0;
      for (int i = 1; i < c->vocab; i++) {
        if (logits[i] > logits[best]) {
          best = i;
        }
      }

      if (best == c->eos_tok) {
        break;
      }

      out[ng++] = best;
      tok = best;
      if (ng >= max_new) {
        break;
      }
    }
  } else {
    ng = -1;
  }

  for (int l = 0; l < AN_MAX_LAYERS; l++) {
    nl_free(sk[l]);
    nl_free(sv[l]);
  }

  nl_free(logits);
  nl_free(scores);

  return ng;
}

int an_decode(an_stream_t *s, int *tokens, int max_tokens) {
  // About 6.5 tokens a second is most speech has; more is a loop
  long cap = (long)((double)s->nenc * 0.02 * 6.5) + 4;
  if (cap < max_tokens) {
    max_tokens = (int)cap;
  }

  int start[1] = {s->c->start_tok};

  return dec_run(s, start, 1, tokens, max_tokens, NULL);
}

int an_decoder_logits(an_stream_t *s, const int *tokens, int n, float *out) {
  return dec_run(s, tokens, n, NULL, 0, out);
}

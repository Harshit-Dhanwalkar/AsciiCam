/*
 * tests/test_asrnet.c - network against a numpy reference
 *
 * A small model (32 wide, 3 + 2 layers) with weights from a PRNG goes through
 * engine and numbers are compared with tests/gen_asrnet_vectors.py, which is
 * written from the published model code and has the same
 * PRNG Weights go through a real safetensors buffer, so reader is on path too
 */

#include "../lib/nolibc.h"
#include "platform.h"

#include "asrnet.h"
#include "nl_math.h"
#include "nn.h"
#include "safetensors.h"

#include "asrnet_vectors.inc"

int memcmp(const void *a, const void *b, size_t n);
static int nl_strstr_ok(const char *h, const char *n) {
  for (; *h; h++) {
    const char *a = h, *b = n;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (!*b) {
      return 1;
    }
  }

  return 0;
}

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#include "asrnet_fixture.h"

static double dabs(double x) { return x < 0 ? -x : x; }

static int near(double a, double b, double tol) {
  return dabs(a - b) <= tol * (1.0 + dabs(b));
}

static an_model_t model;
static st_t st;

static void test_ops(void) {
  float x[4] = {1, 2, 3, 4};
  float y[4] = {0};
  nn_layernorm(y, x, 1, 4, 1e-5f, NULL, NULL);
  CHECK(near(y[0], -1.3416354, 1e-5) && near(y[3], 1.3416354, 1e-5),
        "layernorm %d", (int)(y[0] * 1e6));

  float s[3] = {1000, 1001, 999};
  nn_softmax(s, 3);
  CHECK(near(s[1], 0.6652409557748219, 1e-6) &&
            near(s[0] + s[1] + s[2], 1, 1e-6),
        "softmax is stable for large inputs");

  float g[3] = {-1, 0, 2};
  nn_gelu(g, 3);
  CHECK(near(g[0], -0.15865525393145707, 1e-6) && g[1] == 0 &&
            near(g[2], 1.9544997361036416, 1e-6),
        "gelu");

  float si[2] = {0, 1};
  nn_silu(si, 2);
  CHECK(si[0] == 0 && near(si[1], 0.7310585786300049, 1e-6), "silu");

  float w[6] = {1, 2, 3, 4, 5, 6};
  float b[2] = {10, 20};
  float in[6] = {1, 0, 1, 0, 1, 0};
  float o[4];
  nn_linear(o, in, 2, w, b, 3, 2);
  // Rows [1,0,1] and [0,1,0] times w = [[1,2,3],[4,5,6]] ^T plus b
  CHECK(o[0] == 14 && o[1] == 30 && o[2] == 12 && o[3] == 25,
        "linear %d %d %d %d", (int)o[0], (int)o[1], (int)o[2], (int)o[3]);
  float big_a[37], big_b[37];
  double want = 0;
  for (int i = 0; i < 37; i++) {
    big_a[i] = (float)(i + 1);
    big_b[i] = (float)(i % 5) - 2.0f;
    want += (double)big_a[i] * big_b[i];
  }

  CHECK(near(nn_dot(big_a, big_b, 37), want, 1e-6), "dot with a tail");

  // Rope: position 0 changes nothing, a rotation keeps length
  float r[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  nn_rope(r, 1, 8, 6, 0, 10000.0f);
  CHECK(r[0] == 1 && r[5] == 6, "rope at 0");
  nn_rope(r, 1, 8, 6, 5, 10000.0f);
  CHECK(r[6] == 7 && r[7] == 8, "dims past rot are untouched");
  CHECK(near(r[0] * r[0] + r[1] * r[1], 5, 1e-5), "rope rotates");
}

static void test_encoder_and_decoder(void) {
  size_t size;
  unsigned char *file = build_file(&size, -1, -1, 0);
  char err[128] = {0};
  CHECK(st_parse(&st, file, size, err, sizeof(err)) == 0, "parse: %s", err);
  CHECK(st.n == ndefs, "%d tensors", st.n);
  CHECK(an_load_st(&model, &st, &cfg, err, sizeof(err)) == 0, "load: %s", err);

  an_stream_t *s = an_stream_new_cap(&model, 64);
  CHECK(s != NULL, "stream");
  // In awkward pieces
  size_t at = 0, pieces[5] = {1, 77, 3000, 81, 5};
  int pi = 0;
  while (at < MV_AUDIO) {
    size_t k = pieces[pi++ % 5];
    if (k > MV_AUDIO - at) {
      k = MV_AUDIO - at;
    }

    an_feed(s, audio_f + at, k);
    at += k;
  }

  long before = an_frames(s);
  // Frames that were out before flush must not change by it
  float early[64];
  int ne = before < 8 ? (int)before : 8;
  for (int i = 0; i < ne; i++) {
    early[i] = an_encoder_frame(s, before - 1 - i)[0];
  }

  an_flush(s);
  CHECK(an_frames(s) == MV_FRAMES, "%ld frames (want %d)", an_frames(s),
        MV_FRAMES);
  CHECK(before < MV_FRAMES && before >= MV_FRAMES - 16,
        "lookahead holds back a few frames: %ld of %d", before, MV_FRAMES);
  for (int i = 0; i < ne; i++) {
    CHECK(an_encoder_frame(s, before - 1 - i)[0] == early[i],
          "frame %ld did not change when stream ended", before - 1 - i);
  }

  for (int k = 0; k < MV_NFRAMES; k++) {
    const float *f = an_encoder_frame(s, MV_FRAME_IDX[k]);
    int ok = f != NULL;
    double sum = 0;
    for (int i = 0; ok && i < 8; i++) {
      ok = near(f[i], MV_ENC[k][i], 2e-4);
    }

    for (int i = 0; f && i < cfg.hid; i++) {
      sum += f[i];
    }

    CHECK(ok, "encoder frame %d: %f %f %f vs %f %f %f", MV_FRAME_IDX[k],
          f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0, MV_ENC[k][0], MV_ENC[k][1],
          MV_ENC[k][2]);
    CHECK(near(sum, MV_ENC_SUM[k], 5e-4), "encoder frame %d sum %f vs %f",
          MV_FRAME_IDX[k], sum, MV_ENC_SUM[k]);
  }

  // Decoder: teacher forced logits after each prefix token
  float *lg = nl_malloc((size_t)cfg.vocab * sizeof(float));
  for (int n = 1; n <= 3; n++) {
    int r = an_decoder_logits(s, MV_PREFIX, n, lg);
    CHECK(r == 0, "decoder run %d", r);
    double sum = 0;
    int ok = 1;
    for (int i = 0; i < cfg.vocab; i++) {
      sum += lg[i];
    }

    for (int i = 0; i < 8; i++) {
      ok = ok && near(lg[i], MV_LOGITS[n - 1][i], 5e-4);
    }

    CHECK(ok, "logits after %d tokens: %f %f vs %f %f", n, lg[0], lg[1],
          MV_LOGITS[n - 1][0], MV_LOGITS[n - 1][1]);
    CHECK(near(sum, MV_LOGIT_SUM[n - 1], 5e-4), "logit sum after %d: %f vs %f",
          n, sum, MV_LOGIT_SUM[n - 1]);
  }

  nl_free(lg);

  int toks[16];
  int n = an_decode(s, toks, 6);
  CHECK(n == MV_GEN_N, "greedy gave %d tokens, want %d", n, MV_GEN_N);
  int same = n == MV_GEN_N;
  for (int i = 0; same && i < n; i++) {
    same = toks[i] == MV_GEN[i];
  }

  CHECK(same, "greedy tokens %d %d %d vs %d %d %d", toks[0], toks[1], toks[2],
        MV_GEN[0], MV_GEN[1], MV_GEN[2]);

  // Same audio one sample at a time, and as 16 bit
  an_stream_t *s2 = an_stream_new_cap(&model, 64);
  for (int i = 0; i < MV_AUDIO; i++) {
    an_feed(s2, audio_f + i, 1);
  }

  an_flush(s2);
  int same_enc = an_frames(s2) == an_frames(s);
  for (long i = 0; same_enc && i < an_frames(s); i++) {
    same_enc = memcmp(an_encoder_frame(s, i), an_encoder_frame(s2, i),
                      (size_t)cfg.hid * sizeof(float)) == 0;
  }

  CHECK(same_enc, "one sample at a time gives bit-identical frames");

  // Reset: a stream can be used again with same result
  an_stream_reset(s2);
  CHECK(an_frames(s2) == 0, "reset empties");
  an_feed(s2, audio_f, MV_AUDIO);
  an_flush(s2);
  same_enc = an_frames(s2) == an_frames(s);
  for (long i = 0; same_enc && i < an_frames(s); i++) {
    same_enc = memcmp(an_encoder_frame(s, i), an_encoder_frame(s2, i),
                      (size_t)cfg.hid * sizeof(float)) == 0;
  }

  CHECK(same_enc, "a reset stream repeats itself");

  // A stream that was flushed takes no more audio
  long nf = an_frames(s2);
  an_feed(s2, audio_f, 4000);
  CHECK(an_frames(s2) == nf, "no audio after the end");

  // s16 input is audio / 32768
  for (int i = 0; i < MV_AUDIO; i++) {
    audio_s16[i] = (int16_t)(audio_f[i] * 32768.0f * 0.5f);
  }

  an_stream_reset(s2);
  an_feed_s16(s2, audio_s16, MV_AUDIO);
  an_flush(s2);
  CHECK(an_frames(s2) == nf, "s16 gives the same number of frames");
  // Frame normalisation makes the scale irrelevant except in silence
  CHECK(near(an_encoder_frame(s2, 30)[0], an_encoder_frame(s, 30)[0], 5e-2),
        "half the volume, about the same encoder output");

  an_stream_free(s);
  an_stream_free(s2);

  // Nothing heard: no tokens, no crash
  an_stream_t *e = an_stream_new_cap(&model, 64);
  CHECK(an_decode(e, toks, 6) == -1, "decoding no audio is refused");
  an_flush(e);
  CHECK(an_frames(e) == 0, "no audio, no frames");
  an_stream_free(e);

  // A stream that fills up stops, does not overflow
  an_stream_t *full = an_stream_new_cap(&model, 40);
  static float noise[8000];
  prng_s = 99;
  long guard = 0;
  while (!an_full(full) && guard < 400) {
    for (int i = 0; i < 8000; i++) {
      noise[i] = (float)(prng() - 0.5);
    }

    an_feed(full, noise, 8000);
    guard++;
  }

  CHECK(an_full(full) && an_frames(full) == 40, "fills at %d frames", 40);
  an_feed(full, noise, 8000);
  an_flush(full);
  CHECK(an_frames(full) == 40, "and takes no more");
  an_stream_free(full);

  an_free(&model);
  nl_free(file);
}

static void test_loader(void) {
  char err[128];
  size_t size;
  unsigned char *file;

  // A tensor missing, a tensor of wrong shape
  file = build_file(&size, 5, -1, 0);
  CHECK(st_parse(&st, file, size, err, sizeof(err)) == 0, "parse w/o a tensor");
  CHECK(an_load_st(&model, &st, &cfg, err, sizeof(err)) == -1,
        "missing tensor");
  CHECK(nl_strstr_ok(err, "missing"), "says missing: %s", err);
  st_close(&st);
  nl_free(file);

  file = build_file(&size, -1, 7, 0);
  st_parse(&st, file, size, err, sizeof(err));
  CHECK(an_load_st(&model, &st, &cfg, err, sizeof(err)) == -1, "wrong shape");
  CHECK(nl_strstr_ok(err, "wrong shape"), "says shape: %s", err);
  st_close(&st);
  nl_free(file);

  // F16 and BF16 convert to float
  CHECK(st_f16_to_f32(0x3c00) == 1.0f && st_f16_to_f32(0xc000) == -2.0f &&
            st_f16_to_f32(0x0000) == 0.0f && st_f16_to_f32(0x3555) > 0.333f &&
            st_f16_to_f32(0x3555) < 0.3334f,
        "f16 values");
  CHECK(st_f16_to_f32(0x0001) > 5.9e-8f && st_f16_to_f32(0x0001) < 6.0e-8f,
        "f16 smallest subnormal");
  CHECK(st_f16_to_f32(0x7bff) == 65504.0f, "f16 largest");
  float inf = st_f16_to_f32(0x7c00);
  CHECK(inf > 1e38f, "f16 infinity");

  file = build_file(&size, -1, -1, 2);
  CHECK(st_parse(&st, file, size, err, sizeof(err)) == 0, "bf16 file parses");
  const st_tensor_t *t = st_find(&st, "model.encoder.final_norm.gamma");
  CHECK(t && t->dtype == ST_BF16 && t->numel == 32, "bf16 tensor");
  const float *f = t ? st_f32(&st, t) : NULL;
  CHECK(f != NULL, "bf16 converts");
  st_close(&st);
  nl_free(file);

  // other dtypes are listed but not loadable
  const char *hdr =
      "{\"a\":{\"dtype\":\"I64\",\"shape\":[2],\"data_offsets\":[0,16]},"
      "\"__metadata__\":{\"format\":\"pt\"}}";
  size_t hl = nl_strlen(hdr);
  unsigned char buf[8 + 128 + 16];
  nl_memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 8; i++) {
    buf[i] = (unsigned char)((hl >> (8 * i)) & 255);
  }

  nl_memcpy(buf + 8, hdr, hl);
  CHECK(st_parse(&st, buf, 8 + hl + 16, err, sizeof(err)) == 0,
        "I64 parses: %s", err);
  CHECK(st.n == 1 && st_f32(&st, &st.t[0]) == NULL, "I64 is not loadable");
  st_close(&st);
}

static void put_header(unsigned char *buf, const char *hdr, size_t total) {
  size_t hl = nl_strlen(hdr);
  nl_memset(buf, 0, total);
  for (int i = 0; i < 8; i++) {
    buf[i] = (unsigned char)((hl >> (8 * i)) & 255);
  }

  nl_memcpy(buf + 8, hdr, hl);
}

static void test_malformed(void) {
  char err[128];
  static unsigned char buf[4096];
  struct {
    const char *hdr;
    size_t data;
    const char *why;
  } bad[] = {
      {"", 0, "empty header"},
      {"{", 0, "unterminated"},
      {"[]", 0, "array not object"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,8]}} x", 8,
       "text after"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,4]}}", 8,
       "size vs shape"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,80]}}", 8,
       "range past the end"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[8,0]}}", 8,
       "range backwards"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[99999999999999999999],\"data_"
       "offsets\":[0,0]}}",
       0, "number too large"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[4294967296,4294967296,4294967296],"
       "\"data_offsets\":[0,0]}}",
       0, "numel overflow"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[1,1,1,1,1,1,1],\"data_offsets\":["
       "0,4]}}",
       4, "too many dims"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[1]}}", 4, "no offsets"},
      {"{\"a\":{}}", 0, "empty entry"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]},\"a\":{"
       "\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}",
       4, "duplicate"},
      {"{\"\\u0041\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}",
       4, "unicode escape"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}", 4,
       "missing close"},
      {"{\"a\":{\"dtype\":\"F32\",\"shape\":[-1],\"data_offsets\":[0,4]}}", 4,
       "negative"},
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    size_t hl = nl_strlen(bad[i].hdr);
    put_header(buf, bad[i].hdr, 8 + hl + bad[i].data);
    st_t t;
    int rc = st_parse(&t, buf, 8 + hl + bad[i].data, err, sizeof(err));
    CHECK(rc == -1 && err[0], "%s refused (%s)", bad[i].why, err);
    if (rc == 0) {
      st_close(&t);
    }
  }

  // header length lies
  st_t t;
  nl_memset(buf, 0, 16);
  buf[0] = 0xff;
  buf[1] = 0xff;
  buf[7] = 0x7f;
  CHECK(st_parse(&t, buf, 16, err, sizeof(err)) == -1, "huge header length");
  CHECK(st_parse(&t, buf, 5, err, sizeof(err)) == -1, "5 byte file");
  CHECK(st_parse(&t, buf, 0, err, sizeof(err)) == -1, "empty file");
  CHECK(st_open(&t, "/tmp/does/not/exist.safetensors", err, sizeof(err)) == -1,
        "missing file");

  // an empty tensor list is a valid file
  put_header(buf, "{}", 10);
  CHECK(st_parse(&t, buf, 10, err, sizeof(err)) == 0 && t.n == 0, "no tensors");
  st_close(&t);

  // random damage to a valid file never crashes, whatever it says
  size_t size;
  unsigned char *file = build_file(&size, -1, -1, 0);
  prng_s = 4242;
  size_t hl = 0;
  for (int i = 7; i >= 0; i--) {
    hl = hl << 8 | file[i];
  }

  for (int round = 0; round < 400; round++) {
    unsigned char saved[8];
    size_t at = (size_t)(prng() * (double)(8 + hl));
    size_t ln = round % 3 + 1;
    for (size_t k = 0; k < ln; k++) {
      saved[k] = file[at + k];
      file[at + k] = (unsigned char)(prng() * 256);
    }

    if (st_parse(&t, file, size, err, sizeof(err)) == 0) {
      an_model_t mm;
      if (an_load_st(&mm, &t, &cfg, err, sizeof(err)) == 0) {
        nl_memset(&mm, 0,
                  sizeof(mm)); // loaded: its owned arrays leak in a test
      }

      st_close(&t);
    }

    for (size_t k = 0; k < ln; k++) {
      file[at + k] = saved[k];
    }
  }

  CHECK(1, "random damage to the header");
  nl_free(file);
}

int main(void) {
  make_defs();
  make_cfg();
  make_audio();
  test_math();
  test_encoder_and_decoder();
  test_loader();
  test_malformed();
  fprintf(stderr, "moonshine: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
iff-- git a / C / tests / test_tokdec.c b / C / tests /
    test_tokdec.c ew file mode 100644 ndex 0000000..a8c196a --/ dev / null++ b /
    C / tests / test_tokdec.c @-0,
    0 + 1,
    142 @ @
#include "nolibc.h"
#include "tokdec.h"

    static int pass_n,
    fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static const char *J =
    "{\"version\":\"1.0\",\"added_tokens\":[{\"id\":0,\"content\":\"<unk>\","
    "\"special\":true},{\"id\":1,\"content\":\"<s>\",\"special\":true},"
    "{\"id\":2,\"content\":\"</s>\",\"special\":true}],"
    "\"normalizer\":{\"type\":\"Sequence\",\"pattern\":\"a\\\"b}\"},"
    "\"model\":{\"type\":\"BPE\",\"dropout\":null,\"vocab\":{"
    "\"<unk>\":0,\"<s>\":1,\"</s>\":2,\"\\u2581hello\":3,\"\\u2581wor\":4,"
    "\"ld\":5,\"<0xC3>\":6,\"<0xA9>\":7,\"\\u2581\":8,\"!\":9,\"<0xE2>\":10,"
    "\"caf\\u00e9\":11,\"\\ud83d\\ude00\":12,\"<0xFF>\":13},\"merges\":[]}}";

static size_t text(const tokdec_t *t, const int *ids, int n, char *out,
                   size_t cap) {
  return tokdec_text(t, ids, n, out, cap);
}

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

int main(void) {
  tokdec_t t;
  char err[96], out[128];
  CHECK(tokdec_parse(&t, J, nl_strlen(J), err, sizeof(err)) == 0, "parse: %s",
        err);
  CHECK(t.n == 14, "n = %d", t.n);

  int a[] = {1, 3, 4, 5, 9, 2};
  text(&t, a, 6, out, sizeof(out));
  CHECK(streq(out, "hello world!"), "plain: '%s'", out);

  // Byte fallback spells é as C3 A9
  int b[] = {3, 8, 6, 7};
  text(&t, b, 4, out, sizeof(out));
  CHECK(streq(out, "hello \xC3\xA9"), "bytes: '%s'", out);

  // Vocab keeps \u escapes and surrogate pairs
  int c[] = {11, 12};
  text(&t, c, 2, out, sizeof(out));
  CHECK(streq(out, "caf\xC3\xA9\xF0\x9F\x98\x80"), "escapes: '%s'", out);

  // Half a character becomes '?', never invalid UTF-8
  int d[] = {3, 6};
  text(&t, d, 2, out, sizeof(out));
  CHECK(streq(out, "hello?"), "half: '%s'", out);
  int e[] = {10, 13};
  text(&t, e, 2, out, sizeof(out));
  CHECK(streq(out, "??"), "bad bytes: '%s'", out);

  // Unknown or out of range ids print nothing
  int f[] = {3, 99, -1, 70000, 4};
  text(&t, f, 5, out, sizeof(out));
  CHECK(streq(out, "hello wor"), "range: '%s'", out);

  // Output is cut to buffer and stays terminated
  char small[8];
  size_t n = text(&t, a, 6, small, sizeof(small));
  CHECK(n == 6 && streq(small, "hello "), "cut: '%s' %d", small, (int)n);
  CHECK(text(&t, a, 6, out, 0) == 0, "no room");
  CHECK(text(&t, a, 0, out, sizeof(out)) == 0 && out[0] == 0, "empty");
  tokdec_free(&t);

  // Malformed files are refused, none crash
  const char *bad[] = {
      "",
      "[]",
      "{",
      "{\"model\":{\"vocab\":{\"a\":1,\"b\":}}}",
      "{\"model\":{\"vocab\":{\"a\":1,\"a2\":1}}}",
      "{\"model\":{\"vocab\":{\"a\":99999999}}}",
      "{\"model\":{\"vocab\":{}}}",
      "{\"model\":{\"vocab\":{\"a\\u12\":1,\"b\":2}}}",
      "{\"model\":{\"vocab\":{\"\\ud800x\":1,\"b\":2}}}",
      "{\"added_tokens\":[{\"content\":\"x\"}],\"model\":{\"vocab\":{\"a\":0,"
      "\"b\":1}}}",
      "{\"model\":{\"type\":\"BPE\"}}",
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    int r = tokdec_parse(&t, bad[i], nl_strlen(bad[i]), err, sizeof(err));
    CHECK(r == -1 && err[0], "bad %u refused (%d) %s", i, r, err);
    if (r == 0) {
      tokdec_free(&t);
    }
  }

  // Every cut of a good file either parses to same thing or is refused
  size_t len = nl_strlen(J);
  int parsed_early = 0;
  for (size_t k = 0; k < len; k++) {
    int r = tokdec_parse(&t, J, k, err, sizeof(err));
    if (r == 0) {
      parsed_early++;
      tokdec_free(&t);
    }
  }

  CHECK(parsed_early == 0, "%d truncated files parsed", parsed_early);

  // damaged bytes never crash
  unsigned s = 12345u;
  char *buf = nl_malloc(len + 1);
  for (int it = 0; it < 3000; it++) {
    nl_memcpy(buf, J, len + 1);
    for (int k = 0; k < 3; k++) {
      s ^= s << 13u;
      s ^= s >> 17u;
      s ^= s << 5u;
      buf[s % len] = (char)(s >> 8u);
    }

    if (tokdec_parse(&t, buf, len, err, sizeof(err)) == 0) {
      int ids[3] = {3, 4, 5};
      text(&t, ids, 3, out, sizeof(out));
      tokdec_free(&t);
    }
  }

  nl_free(buf);
  fprintf(stderr, "tokdec: %d passed, %d failed\n", pass_n, fail_n);
  return fail_n != 0;
}

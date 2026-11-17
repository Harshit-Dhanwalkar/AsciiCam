/*
 * tests/test_asr.c - utterance cutting and partial/final lines of asr.c
 *
 * Model is tiny random one of test_asrnet.c, so what it "says" is is noise made
 * of pieces w3..w49; what is tested is when lines come out, that they do not
 * depend on how audio was chunked, and that nothing breaks on clicks, long
 * speech and end of stream
 */

#include "nolibc.h"
#include "platform.h"

#include "asr.h"
#include "asrnet.h"
#include "nl_math.h"
#include "nn.h"
#include "safetensors.h"
#include "speech.h"
#include "tokdec.h"

#include "asrnet_vectors.inc"

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

static an_model_t model;
static st_t st;
static tokdec_t tok;

static void make_tokenizer(void) {
  static char json[8192];
  size_t n = 0;
  n += (size_t)nl_snprintf(json + n, sizeof(json) - n,
                           "{\"model\":{\"vocab\":{\"<pad>\":0,\"<s>\":1,"
                           "\"</s>\":2");
  for (int i = 3; i < 50; i++) {
    n += (size_t)nl_snprintf(json + n, sizeof(json) - n, ",\"\\u2581w%d\":%d",
                             i, i);
  }

  n += (size_t)nl_snprintf(json + n, sizeof(json) - n,
                           "}},\"added_tokens\":[{\"id\":0,\"content\":"
                           "\"<pad>\",\"special\":true},{\"id\":1,\"content\":"
                           "\"<s>\",\"special\":true},{\"id\":2,\"content\":"
                           "\"</s>\",\"special\":true}]}");
  char err[96];
  CHECK(tokdec_parse(&tok, json, n, err, sizeof(err)) == 0, "tokenizer: %s",
        err);
}

// What came out
typedef struct {
  int n;
  char kind[64];
  char text[64][ASR_TEXT];
} log_t;

static void on_text(void *user, int final, const char *text) {
  log_t *l = user;
  if (l->n < 64) {
    l->kind[l->n] = final ? 'F' : 'P';
    nl_strncpy_safe(l->text[l->n], text, ASR_TEXT);
    l->n++;
  }
}

static unsigned rs = 777;
static int rnd(void) {
  rs ^= rs << 13;
  rs ^= rs >> 17;
  rs ^= rs << 5;

  return (int)(rs >> 8) & 0xffff;
}

#define RATE 16000
static int16_t sig[RATE * 45];
static size_t sig_n;

static void quiet(int ms) {
  for (int i = 0; i < ms * 16; i++) {
    sig[sig_n++] = (int16_t)(rnd() % 7 - 3);
  }
}

// Loud, voiced-ish: a tone with harmonics and noise
static void talk(int ms) {
  for (int i = 0; i < ms * 16; i++) {
    double t = (double)i / RATE;
    double v = 6000.0 * nl_sin(6.2831853 * 180.0 * t) +
               3000.0 * nl_sin(6.2831853 * 540.0 * t) + (rnd() % 2000 - 1000);
    sig[sig_n++] = (int16_t)v;
  }
}

static long last_frames, last_blocks;

static int run(log_t *l, const int16_t *s, size_t n, const size_t *pieces,
               int npieces, int do_flush) {
  static asr_t a;
  asr_init(&a);
  nl_memset(l, 0, sizeof(*l));
  if (asr_use(&a, &model, &tok) != 0) {
    return -1;
  }

  asr_set_text(&a, on_text, l);
  size_t at = 0;
  int pi = 0;
  while (at < n) {
    size_t k = pieces ? pieces[pi++ % npieces] : n;
    if (k > n - at) {
      k = n - at;
    }

    asr_feed(&a, s + at, k);
    at += k;
  }

  if (do_flush) {
    asr_flush(&a);
  }

  int utt = a.utterances;
  last_frames = a.last_frames;
  last_blocks = a.last_blocks;
  asr_close(&a);

  return utt;
}

static int count(const log_t *l, char kind) {
  int c = 0;
  for (int i = 0; i < l->n; i++) {
    c += l->kind[i] == kind;
  }

  return c;
}

static int same(const log_t *a, const log_t *b) {
  if (a->n != b->n) {
    return 0;
  }

  for (int i = 0; i < a->n; i++) {
    if (a->kind[i] != b->kind[i] || nl_strcmp(a->text[i], b->text[i]) != 0) {
      return 0;
    }
  }

  return 1;
}

static int same_ids(const int *a, int na, const int *b, int nb) {
  if (na != nb) {
    return 0;
  }

  for (int i = 0; i < na; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
  }

  return 1;
}

// True when some word of text is said n times in a row
static int words_repeat(const char *text, int n) {
  char prev[ASR_TEXT] = "";
  int run = 0;
  const char *p = text;
  while (*p) {
    const char *e = p;
    while (*e && *e != ' ') {
      e++;
    }

    size_t len = (size_t)(e - p);
    char word[ASR_TEXT];
    if (len >= sizeof(word)) {
      len = sizeof(word) - 1;
    }

    for (size_t k = 0; k < len; k++) {
      word[k] = p[k];
    }

    word[len] = 0;
    if (len > 0 && nl_strcmp(word, prev) == 0) {
      run++;
    } else {
      run = 1;
    }

    if (run >= n) {
      return 1;
    }

    nl_strncpy_safe(prev, word, sizeof(prev));
    p = *e ? e + 1 : e;
  }

  return 0;
}

static void test_trim(void) {
  int out[64];
  int in1[] = {1, 2, 3, 3, 3, 3};
  int want1[] = {1, 2, 3, 3};
  int n = asr_trim_repeats(in1, 6, out);
  CHECK(same_ids(out, n, want1, 4), "run of 1 cut to 2: %d ids", n);

  int in2[] = {4, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3};
  int want2[] = {4, 1, 2, 3, 1, 2, 3};
  n = asr_trim_repeats(in2, 13, out);
  CHECK(same_ids(out, n, want2, 7), "block of 3 repeated: cut to 2 copies");

  int in3[] = {5, 6, 5, 6, 7};
  n = asr_trim_repeats(in3, 5, out);
  CHECK(same_ids(out, n, in3, 5), "two copies are kept as they are");

  int in4[] = {9, 9, 9};
  n = asr_trim_repeats(in4, 3, out);
  CHECK(same_ids(out, n, in4, 2), "three in a row: the third goes");

  int in5[] = {1, 2, 1, 2, 1, 2, 1, 2, 1};
  n = asr_trim_repeats(in5, 9, out);
  CHECK(n == 5, "alternating loop cut to two copies: %d", n);
  CHECK(asr_trim_repeats(in5, 0, out) == 0, "empty");

  // Nothing repeats: unchanged
  int in6[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  n = asr_trim_repeats(in6, 10, out);
  CHECK(same_ids(out, n, in6, 10), "no repeats: unchanged");

  // A loop longer than longest block is kept (ASR_REPEAT_LEN is 8)
  int in7[18];
  for (int i = 0; i < 18; i++) {
    in7[i] = i % 9 + 20;
  }

  n = asr_trim_repeats(in7, 18, out);
  CHECK(n == 18, "period 9 is not looked at: %d", n);
}

static void test_cutting(void) {
  static log_t l, l2;

  // Quiet, a 1.6 s Burst, a Pause, a 3.2 s Burst, Quiet
  sig_n = 0;
  quiet(1000);
  talk(1600);
  quiet(1200);
  talk(3200);
  quiet(1500);
  int utt = run(&l, sig, sig_n, NULL, 0, 1);
  CHECK(utt == 2 && count(&l, 'F') == 2, "two utterances: %d, %d finals", utt,
        count(&l, 'F'));
  CHECK(count(&l, 'P') >= 1, "the long one gave partials: %d", count(&l, 'P'));
  // A partial line is only sent when it changed
  for (int i = 1; i < l.n; i++) {
    CHECK(!(l.kind[i] == 'P' && l.kind[i - 1] == 'P' &&
            nl_strcmp(l.text[i], l.text[i - 1]) == 0),
          "partial %d repeats", i);
  }

  // End of utterance is flushed through encoder, so last frames (held back for
  // lookahead) are in: one frame per 20 ms block
  CHECK(last_frames >= last_blocks - 2 && last_frames <= last_blocks + 2,
        "%ld frames for %ld blocks", last_frames, last_blocks);

  // A partial is always followed, sooner or later, by a final
  CHECK(l.n > 0 && l.kind[l.n - 1] == 'F', "ends on a final");
  for (int i = 0; i < l.n; i++) {
    CHECK(l.text[i][0] == 'w', "line %d is text: '%s'", i, l.text[i]);
  }

  // However audio arrives
  size_t odd[] = {1, 319, 320, 321, 4000, 7, 640};
  run(&l2, sig, sig_n, odd, 7, 1);
  CHECK(same(&l, &l2), "chunking changes nothing (%d vs %d lines)", l.n, l2.n);
  size_t one[] = {1};
  run(&l2, sig, sig_n, one, 1, 1);
  CHECK(same(&l, &l2), "one sample at a time gives the same (%d lines)", l2.n);

  // A click is not speech, a short noise is not either
  sig_n = 0;
  quiet(800);
  talk(60);
  quiet(1500);
  utt = run(&l, sig, sig_n, NULL, 0, 1);
  CHECK(utt == 0 && l.n == 0, "a click says nothing: %d", l.n);

  // Steady noise is not speech: floor follows it
  sig_n = 0;
  for (int i = 0; i < 6 * RATE; i++) {
    sig[sig_n++] = (int16_t)(rnd() % 801 - 400);
  }

  utt = run(&l, sig, sig_n, NULL, 0, 1);
  CHECK(utt == 0 && l.n == 0, "steady noise says nothing: %d", l.n);

  // Speech that runs on to end of audio is finished by flush and not before
  sig_n = 0;
  quiet(500);
  talk(2500);
  utt = run(&l, sig, sig_n, NULL, 0, 0);
  CHECK(utt == 0 && count(&l, 'F') == 0, "nothing final before the end");
  utt = run(&l, sig, sig_n, NULL, 0, 1);
  CHECK(utt == 1 && count(&l, 'F') == 1, "flush finishes it: %d", utt);

  // 35 s of talking is cut at 30 s and goes on
  sig_n = 0;
  quiet(300);
  talk(35000);
  quiet(1000);
  utt = run(&l, sig, sig_n, NULL, 0, 1);
  CHECK(utt >= 2, "long speech is cut: %d utterances", utt);

  // No audio at all
  utt = run(&l, sig, 0, NULL, 0, 1);
  CHECK(utt == 0 && l.n == 0, "empty");
}

static void test_tap(void) {
  // speech.h hands samples to asr through sp_on_pcm
  static sp_t sp;
  static asr_t a;
  static log_t l;
  sp_init(&sp);
  asr_init(&a);
  nl_memset(&l, 0, sizeof(l));
  CHECK(asr_use(&a, &model, &tok) == 0, "asr_use");
  asr_set_text(&a, on_text, &l);
  asr_attach(&a, &sp);
  sig_n = 0;
  quiet(600);
  talk(1800);
  quiet(900);
  sp_feed(&sp, sig, sig_n);
  sp_flush(&sp);
  CHECK(count(&l, 'F') == 1, "through sp_feed: %d finals", count(&l, 'F'));
  CHECK(sp.utterances == 0, "the template recogniser stayed out");
  asr_close(&a);
}

static void test_open(void) {
  asr_t a;
  char err[128];
  asr_init(&a);
  CHECK(asr_open(&a, "/nonexistent/model.safetensors", NULL, err,
                 sizeof(err)) == -1 &&
            err[0],
        "missing files refused: %s", err);
  CHECK(asr_open(&a, "/nonexistent/model.safetensors", "/nonexistent/t.json",
                 err, sizeof(err)) == -1,
        "missing tokenizer refused");
  // Never opened: feeding and closing do nothing
  int16_t x[1000] = {0};
  asr_feed(&a, x, 1000);
  asr_flush(&a);
  asr_close(&a);
  CHECK(1, "closed");
}

int main(void) {
  char err[128];
  make_defs();
  make_cfg();
  size_t size;
  unsigned char *file = build_file(&size, -1, -1, 0);
  CHECK(st_parse(&st, file, size, err, sizeof(err)) == 0, "parse: %s", err);
  CHECK(an_load_st(&model, &st, &cfg, err, sizeof(err)) == 0, "load: %s", err);
  make_tokenizer();
  test_trim();
  test_cutting();
  test_tap();
  test_open();
  an_free(&model);
  tokdec_free(&tok);
  nl_free(file);
  fprintf(stderr, "asr: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n != 0;
}

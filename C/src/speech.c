/*
 * src/speech.c - live speech recognition (MFCC + VAD + DTW)
 *
 * NOTE: no libm used: few functions needed (ln, exp, sin, sqrt) are written
 * out below, in double, and only used to build tables and per frame
 */

#include "platform.h"

#include "nolibc.h"

#include "speech.h"

#define SP_O_NONBLOCK 04000
#define SP_INF 1e30f
#define SP_PI 3.14159265358979323846
#define SP_LN2 0.69314718055994531

#define SP_ON_FRAMES 3   // loud frames in a row that start an utterance
#define SP_HANG 30       // quiet frames that end it (300 ms)
#define SP_KEEP_TAIL 3   // of those, kept as end of word
#define SP_MIN_FRAMES 10 // shorter is a click, not a word
#define SP_MIN_LOUD 8    // and so is less than this much actual sound
#define SP_PARTIAL_EVERY 30
#define SP_PARTIAL_MAX 150 // no partials past this: too slow, final soon
#define SP_BLOCK 50
#define SP_MIN_ON_DB 40.0f
#define SP_MIN_OFF_DB 36.0f

/* Maths helpers */

static double sp_ln(double x) {
  if (x <= 0) {
    return -700.0;
  }

  union {
    double d;
    uint64_t u;
  } v = {x};
  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  int e = (int)((v.u >> 52) & 0x7ffu) - 1023;
  v.u = (v.u & 0xfffffffffffffULL) | 0x3ff0000000000000ULL;
  double m = v.d;
  if (m > 1.41421356237) {
    m *= 0.5;
    e++;
  }

  double s = (m - 1) / (m + 1);
  double s2 = s * s;
  double r =
      s *
      (1 + s2 * (1 / 3.0 +
                 s2 * (1 / 5.0 + s2 * (1 / 7.0 + s2 * (1 / 9.0 + s2 / 11.0)))));

  return 2 * r + e * SP_LN2;
}

static double sp_exp(double x) {
  int k = (int)(x / SP_LN2 + (x < 0 ? -0.5 : 0.5));
  double r = x - k * SP_LN2;
  double t = 1, s = 1;
  for (int i = 1; i < 14; i++) {
    t *= r / i;
    s += t;
  }

  while (k > 0) {
    s *= 2;
    k--;
  }

  while (k < 0) {
    s *= 0.5;
    k++;
  }

  return s;
}

static double sp_sin(double x) {
  double k = x / (2 * SP_PI);
  long n = (long)(k + (k < 0 ? -0.5 : 0.5));
  x -= (double)n * 2 * SP_PI;

  double x2 = x * x, t = x, r = x;
  for (int i = 1; i < 12; i++) {
    t *= -x2 / ((2.0 * i) * (2.0 * i + 1));
    r += t;
  }

  return r;
}

static double sp_cos(double x) { return sp_sin(x + SP_PI / 2); }

static float sp_sqrtf(float x) {
  if (x <= 0) {
    return 0;
  }

  union {
    float f;
    uint32_t u;
  } v = {x};
  v.u = (v.u >> 1u) + 0x1fbd1df5u;
  float y = v.f;
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);

  return y;
}

/* Tables, built once */

static int tab_ready;
static float tab_win[SP_FRAME];
static float tab_cos[SP_NFFT / 2], tab_sin[SP_NFFT / 2];
static unsigned short tab_rev[SP_NFFT];
static float tab_fb[SP_NMEL][SP_NFFT / 2 + 1];
static float tab_dct[SP_NCEP][SP_NMEL];
static float tab_lift[SP_NCEP];

static double hz_to_mel(double f) { return 1127.0 * sp_ln(1.0 + f / 700.0); }
static double mel_to_hz(double m) { return 700.0 * (sp_exp(m / 1127.0) - 1.0); }

static void tables(void) {
  if (tab_ready) {
    return;
  }

  for (int i = 0; i < SP_FRAME; i++) {
    tab_win[i] = (float)(0.54 - 0.46 * sp_cos(2 * SP_PI * i / (SP_FRAME - 1)));
  }

  for (int i = 0; i < SP_NFFT / 2; i++) {
    tab_cos[i] = (float)sp_cos(2 * SP_PI * i / SP_NFFT);
    tab_sin[i] = (float)-sp_sin(2 * SP_PI * i / SP_NFFT);
  }

  int bits = 0;
  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  while ((1 << bits) < SP_NFFT) {
    bits++;
  }

  for (int i = 0; i < SP_NFFT; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) {
      // NOLINTNEXTLINE(bugprone-signed-bitwise)
      if (i & (1 << b)) {
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        r |= 1 << (bits - 1 - b);
      }
    }

    tab_rev[i] = (unsigned short)r;
  }

  // Triangular filters, evenly spaced on mel scale, 80 Hz .. 7.6 kHz
  double lo = hz_to_mel(80.0), hi = hz_to_mel(7600.0);
  double pts[SP_NMEL + 2];
  for (int i = 0; i < SP_NMEL + 2; i++) {
    pts[i] = mel_to_hz(lo + (hi - lo) * i / (SP_NMEL + 1));
  }

  for (int m = 0; m < SP_NMEL; m++) {
    for (int k = 0; k <= SP_NFFT / 2; k++) {
      double f = (double)k * SP_RATE / SP_NFFT;
      double w = 0;
      if (f > pts[m] && f <= pts[m + 1]) {
        w = (f - pts[m]) / (pts[m + 1] - pts[m]);
      } else if (f > pts[m + 1] && f < pts[m + 2]) {
        w = (pts[m + 2] - f) / (pts[m + 2] - pts[m + 1]);
      }

      tab_fb[m][k] = (float)w;
    }
  }

  // c1..c12 of DCT-II, with a sine lifter so that coefficients weigh about same
  for (int k = 0; k < SP_NCEP; k++) {
    for (int m = 0; m < SP_NMEL; m++) {
      tab_dct[k][m] = (float)sp_cos(SP_PI * (k + 1) * (m + 0.5) / SP_NMEL);
    }

    tab_lift[k] = (float)(1.0 + 11.0 * sp_sin(SP_PI * (k + 1) / 22.0));
  }

  tab_ready = 1;
}

static void fft(float *re, float *im) {
  for (int i = 0; i < SP_NFFT; i++) {
    int j = tab_rev[i];
    if (j > i) {
      float t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
  }

  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  for (int len = 2; len <= SP_NFFT; len <<= 1) {
    int half = len / 2, step = SP_NFFT / len;
    for (int i = 0; i < SP_NFFT; i += len) {
      for (int j = 0; j < half; j++) {
        float wr = tab_cos[j * step], wi = tab_sin[j * step];
        int a = i + j, b = a + half;
        float xr = re[b] * wr - im[b] * wi;
        float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
}

// One frame of SP_FRAME samples -> cepstra and loudness (dB)
static void frame_features(const float *x, float *ceps, float *db) {
  static float re[SP_NFFT], im[SP_NFFT];

  double en = 0;
  for (int i = 0; i < SP_FRAME; i++) {
    en += (double)x[i] * x[i];
  }

  *db = (float)(4.342944819032518 * sp_ln(en / SP_FRAME + 1.0)); // 10 log10

  for (int i = 0; i < SP_FRAME; i++) {
    float prev = i ? x[i - 1] : x[0];
    re[i] = (x[i] - 0.97f * prev) * tab_win[i];
    im[i] = 0;
  }

  for (int i = SP_FRAME; i < SP_NFFT; i++) {
    re[i] = im[i] = 0;
  }

  fft(re, im);

  float lm[SP_NMEL];
  for (int m = 0; m < SP_NMEL; m++) {
    float acc = 0;
    for (int k = 0; k <= SP_NFFT / 2; k++) {
      float w = tab_fb[m][k];
      if (w > 0) {
        acc += w * (re[k] * re[k] + im[k] * im[k]);
      }
    }

    lm[m] = (float)sp_ln((double)acc + 1.0);
  }

  for (int k = 0; k < SP_NCEP; k++) {
    float acc = 0;
    for (int m = 0; m < SP_NMEL; m++) {
      acc += tab_dct[k][m] * lm[m];
    }

    ceps[k] = acc * tab_lift[k] * (1.0f / SP_NMEL);
  }
}

/* Matching */

static float local_dist(const float *a, const float *b) {
  float d = 0;
  for (int k = 0; k < SP_NCEP; k++) {
    float x = a[k] - b[k];
    d += x * x;
  }

  return sp_sqrtf(d);
}

float sp_dtw(const float *q, int n, const float *t, int m, int open_end) {
  if (n < 1 || m < 1 || n > SP_MAXFR || m > SP_MAXFR) {
    return SP_INF;
  }

  if (!open_end && (n > 2 * m || m > 2 * n)) {
    return SP_INF;
  }

  static float rows[2][SP_MAXFR + 1];
  float *prev = rows[0], *cur = rows[1];
  for (int j = 0; j <= m; j++) {
    prev[j] = SP_INF;
  }

  prev[0] = 0; // cell (-1,-1)

  int w = (n > m ? n : m) / 4 + 3;
  for (int i = 0; i < n; i++) {
    // Band around diagonal; an open ended match has no known end, so it follows
    // j = i and gets wider as it goes
    int jc, jw;
    if (open_end) {
      jc = i;
      jw = i / 2 + 10;
    } else {
      jc = n > 1 ? i * (m - 1) / (n - 1) : 0;
      jw = w;
    }

    int j0 = jc - jw, j1 = jc + jw;
    if (j0 < 0) {
      j0 = 0;
    }

    if (j1 > m - 1) {
      j1 = m - 1;
    }

    for (int j = 0; j <= m; j++) {
      cur[j] = SP_INF;
    }

    for (int j = j0; j <= j1; j++) {
      float d = local_dist(q + i * SP_NCEP, t + j * SP_NCEP);
      // symmetric step pattern: diagonal counts double so that cost does not
      // depend on which way a path bends
      float a = prev[j] + 2 * d; // (i-1, j-1), prev[] is shifted by one
      float b = prev[j + 1] + d; // (i-1, j)
      float c = cur[j] + d;      // (i, j-1)
      float best = a < b ? a : b;
      if (c < best) {
        best = c;
      }

      cur[j + 1] = best;
    }

    float *tmp = prev;
    prev = cur;
    cur = tmp;
  }

  if (!open_end) {
    float v = prev[m];
    if (v >= SP_INF) {
      return SP_INF;
    }

    return v / (float)(n + m);
  }

  // Template may end anywhere past half of query
  float best = SP_INF;
  int jlo = n / 2;
  int jhi = n * 3 / 2 + 5;
  if (jhi > m) {
    jhi = m;
  }

  for (int j = jlo; j < jhi; j++) {
    float v = prev[j + 1];
    if (v >= SP_INF) {
      continue;
    }

    v /= (float)(n + j + 1);
    if (v < best) {
      best = v;
    }
  }

  return best;
}

static int same_word(const char *a, const char *b) {
  return nl_strcmp(a, b) == 0;
}

static int match_impl(const sp_t *s, const float *f, int nfr, int open_end,
                      float thresh_scale, float *dist, float *margin) {
  int best_i = -1;
  float best = SP_INF, second = SP_INF;
  // per word best: templates of a word are not all next to each other, so look
  // for closest template of another word each time
  float d[SP_MAX_TPL];
  for (int i = 0; i < s->ntpl; i++) {
    d[i] = sp_dtw(f, nfr, s->tpl[i].f, s->tpl[i].nfr, open_end);
    if (d[i] < best) {
      best = d[i];
      best_i = i;
    }
  }

  if (best_i < 0 || best >= SP_INF) {
    return -1;
  }

  for (int i = 0; i < s->ntpl; i++) {
    if (!same_word(s->tpl[i].word, s->tpl[best_i].word) && d[i] < second) {
      second = d[i];
    }
  }

  if (dist) {
    *dist = best;
  }

  if (margin) {
    *margin = second >= SP_INF ? SP_INF : second / (best + 1e-6f);
  }

  if (best > s->thresh * thresh_scale) {
    return -1;
  }

  if (second < SP_INF && second <= best * s->margin) {
    return -1;
  }

  return best_i;
}

int sp_match(const sp_t *s, const float *f, int nfr, float *dist,
             float *margin) {
  return match_impl(s, f, nfr, 0, 1.0f, dist, margin);
}

/* Templates */

void sp_init(sp_t *s) {
  tables();
  nl_memset(s, 0, sizeof(*s));
  s->thresh = 4.0f;
  s->margin = 1.5f;
  for (int i = 0; i < 6; i++) {
    s->blockmin[i] = 1e9f;
  }

  s->curmin = 1e9f;
}

void sp_free(sp_t *s) {
  for (int i = 0; i < s->ntpl; i++) {
    nl_free(s->tpl[i].f);
    s->tpl[i].f = NULL;
  }

  s->ntpl = 0;
}

void sp_set_text(sp_t *s, sp_text_fn fn, void *user) {
  s->text = fn;
  s->text_user = user;
}

void sp_on_utterance(sp_t *s, sp_utt_fn fn, void *user) {
  s->utt = fn;
  s->utt_user = user;
}

int sp_count(const sp_t *s, const char *word) {
  int n = 0;
  for (int i = 0; i < s->ntpl; i++) {
    if (same_word(s->tpl[i].word, word)) {
      n++;
    }
  }

  return n;
}

int sp_words(const sp_t *s) {
  int n = 0;
  for (int i = 0; i < s->ntpl; i++) {
    int seen = 0;
    for (int j = 0; j < i; j++) {
      if (same_word(s->tpl[j].word, s->tpl[i].word)) {
        seen = 1;
        break;
      }
    }

    if (!seen) {
      n++;
    }
  }

  return n;
}

static void drop(sp_t *s, int i) {
  nl_free(s->tpl[i].f);
  for (int k = i; k + 1 < s->ntpl; k++) {
    s->tpl[k] = s->tpl[k + 1];
  }

  s->ntpl--;
  s->tpl[s->ntpl].f = NULL;
}

int sp_remove(sp_t *s, const char *word) {
  int n = 0;
  for (int i = s->ntpl - 1; i >= 0; i--) {
    if (same_word(s->tpl[i].word, word)) {
      drop(s, i);
      n++;
    }
  }

  return n;
}

int sp_add(sp_t *s, const char *word, const float *f, int nfr) {
  if (!word || !*word || nl_strlen(word) >= SP_WORD || nfr < SP_MIN_FRAMES ||
      nfr > SP_MAXFR) {
    return -1;
  }

  if (sp_count(s, word) >= SP_PER_WORD) {
    for (int i = 0; i < s->ntpl; i++) {
      if (same_word(s->tpl[i].word, word)) {
        drop(s, i); // oldest comes first
        break;
      }
    }
  }

  if (s->ntpl >= SP_MAX_TPL) {
    return -1;
  }

  float *copy = nl_malloc((size_t)nfr * SP_NCEP * sizeof(float));
  if (!copy) {
    return -1;
  }

  nl_memcpy(copy, f, (size_t)nfr * SP_NCEP * sizeof(float));
  sp_tpl_t *t = &s->tpl[s->ntpl++];
  nl_strncpy_safe(t->word, word, SP_WORD);
  t->nfr = nfr;
  t->f = copy;

  return 0;
}

/* Segmentation */

static void emit(sp_t *s, int final, const char *text) {
  if (s->text) {
    s->text(s->text_user, final, text);
  }
}

// Mean normalise n frames in place (copy first)
static void normalise(float *f, int n) {
  for (int k = 0; k < SP_NCEP; k++) {
    float mean = 0;
    for (int i = 0; i < n; i++) {
      mean += f[i * SP_NCEP + k];
    }

    mean /= (float)n;
    for (int i = 0; i < n; i++) {
      f[i * SP_NCEP + k] -= mean;
    }
  }
}

static void end_utterance(sp_t *s, int trim) {
  int n = s->nfr - trim;
  s->speaking = 0;
  s->quiet = 0;
  s->run_on = 0;
  s->pre_n = 0;
  s->pre_i = 0;
  s->part_at = 0;
  s->part_last[0] = 0;
  int loud = s->loud;
  s->loud = 0;
  s->nfr = 0;

  if (n < SP_MIN_FRAMES || loud < SP_MIN_LOUD) {
    return;
  }

  normalise(&s->feat[0][0], n);
  s->utterances++;

  if (s->utt) {
    s->utt(s->utt_user, &s->feat[0][0], n);

    return;
  }

  float dist = 0, margin = 0;
  int i = sp_match(s, &s->feat[0][0], n, &dist, &margin);
  s->last_dist = dist;
  s->last_margin = margin;
  if (i < 0) {
    s->last_word[0] = 0;

    return;
  }

  s->accepted++;
  nl_strncpy_safe(s->last_word, s->tpl[i].word, SP_WORD);
  emit(s, 1, s->tpl[i].word);
}

// Hypothesis from what has been said so far
static void partial(sp_t *s, int n) {
  static float tmp[SP_PARTIAL_MAX + 1][SP_NCEP];
  if (!s->text || s->utt || s->ntpl == 0 || n > SP_PARTIAL_MAX) {
    return;
  }

  nl_memcpy(tmp, s->feat, (size_t)n * SP_NCEP * sizeof(float));
  normalise(&tmp[0][0], n);

  float dist = 0, margin = 0;
  // looser than final: half a word can not be as close as a whole one
  int i = match_impl(s, &tmp[0][0], n, 1, 1.3f, &dist, &margin);
  if (i < 0 || nl_strcmp(s->part_last, s->tpl[i].word) == 0) {
    return;
  }

  nl_strncpy_safe(s->part_last, s->tpl[i].word, SP_WORD);
  char line[SP_WORD + 2];
  nl_snprintf(line, sizeof(line), "%s", s->tpl[i].word);
  emit(s, 0, line);
}

static void on_frame(sp_t *s, const float *x) {
  float ceps[SP_NCEP], db;
  frame_features(x, ceps, &db);

  // Noise floor: quietest of last few seconds
  // It is not followed while someone is talking, or a long sentence would
  // become background
  if (!s->speaking) {
    if (db < s->curmin) {
      s->curmin = db;
    }

    if (++s->curn >= SP_BLOCK) {
      for (int i = 0; i < 5; i++) {
        s->blockmin[i] = s->blockmin[i + 1];
      }

      s->blockmin[5] = s->curmin;
      s->curmin = 1e9f;
      s->curn = 0;
    }
  }

  float nf = s->curmin;
  for (int i = 0; i < 6; i++) {
    if (s->blockmin[i] < nf) {
      nf = s->blockmin[i];
    }
  }

  float on = nf + 10.0f, off = nf + 6.0f;
  if (on < SP_MIN_ON_DB) {
    on = SP_MIN_ON_DB;
  }

  if (off < SP_MIN_OFF_DB) {
    off = SP_MIN_OFF_DB;
  }

  if (!s->speaking) {
    nl_memcpy(s->pre[s->pre_i], ceps, sizeof(ceps));
    s->pre_i = (s->pre_i + 1) % SP_PREROLL;
    if (s->pre_n < SP_PREROLL) {
      s->pre_n++;
    }

    s->run_on = db > on ? s->run_on + 1 : 0;
    if (s->run_on >= SP_ON_FRAMES) {
      s->speaking = 1;
      s->quiet = 0;
      s->loud = SP_ON_FRAMES;
      s->nfr = 0;
      int start = (s->pre_i - s->pre_n + SP_PREROLL) % SP_PREROLL;
      for (int i = 0; i < s->pre_n; i++) {
        nl_memcpy(s->feat[s->nfr++], s->pre[(start + i) % SP_PREROLL],
                  sizeof(ceps));
      }
    }

    return;
  }

  nl_memcpy(s->feat[s->nfr++], ceps, sizeof(ceps));
  s->quiet = db < off ? s->quiet + 1 : 0;
  if (db >= off) {
    s->loud++;
  }

  if (s->quiet >= SP_HANG) {
    int keep = s->quiet < SP_KEEP_TAIL ? s->quiet : SP_KEEP_TAIL;
    end_utterance(s, s->quiet - keep);
  } else if (s->nfr >= SP_MAXFR) {
    end_utterance(s, 0);
  } else if (s->nfr - s->part_at >= SP_PARTIAL_EVERY) {
    s->part_at = s->nfr;
    partial(s, s->nfr - s->quiet);
  }
}

void sp_feed(sp_t *s, const int16_t *pcm, size_t n) {
  for (size_t i = 0; i < n; i++) {
    s->pcm[s->npcm++] = (float)pcm[i];
    if (s->npcm == SP_FRAME) {
      on_frame(s, s->pcm);
      for (int k = 0; k < SP_FRAME - SP_HOP; k++) {
        s->pcm[k] = s->pcm[k + SP_HOP];
      }

      s->npcm = SP_FRAME - SP_HOP;
    }
  }
}

void sp_flush(sp_t *s) {
  if (s->speaking) {
    int keep = s->quiet < SP_KEEP_TAIL ? s->quiet : SP_KEEP_TAIL;
    end_utterance(s, s->quiet - keep);
  }
}

void sp_reset(sp_t *s) {
  s->npcm = 0;
  s->speaking = 0;
  s->run_on = 0;
  s->quiet = 0;
  s->loud = 0;
  s->nfr = 0;
  s->pre_n = 0;
  s->pre_i = 0;
  s->part_at = 0;
  s->part_last[0] = 0;
  s->curmin = 1e9f;
  s->curn = 0;
  for (int i = 0; i < 6; i++) {
    s->blockmin[i] = 1e9f;
  }
}

/* Template file: "ASPT", u32 version 1, u32 count; then per template
 * u8 len, word, u16 nfr, nfr * SP_NCEP floats (little endian machines) */

#define SP_MAGIC 0x54505341u // "ASPT"
#define SP_FILE_MAX (6u << 20)

static void put32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    p[i] = (unsigned char)(v >> (8 * i));
  }
}

static uint32_t get32(const unsigned char *p) {
  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         // NOLINTNEXTLINE(bugprone-signed-bitwise)
         (uint32_t)p[3] << 24;
}

static void make_dirs(const char *path) {
  char tmp[SP_PATH];
  nl_strncpy_safe(tmp, path, sizeof(tmp));
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
}

int sp_save(const sp_t *s, const char *path) {
  if (nl_strlen(path) + 5 >= SP_PATH) {
    return -1;
  }

  size_t cap = 12;
  for (int i = 0; i < s->ntpl; i++) {
    cap += 3 + nl_strlen(s->tpl[i].word) +
           (size_t)s->tpl[i].nfr * SP_NCEP * sizeof(float);
  }

  unsigned char *buf = nl_malloc(cap);
  if (!buf) {
    return -1;
  }

  size_t n = 0;
  put32(buf, SP_MAGIC);
  put32(buf + 4, 1);
  put32(buf + 8, (uint32_t)s->ntpl);
  n = 12;
  for (int i = 0; i < s->ntpl; i++) {
    size_t wl = nl_strlen(s->tpl[i].word);
    buf[n++] = (unsigned char)wl;
    nl_memcpy(buf + n, s->tpl[i].word, wl);
    n += wl;
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    buf[n++] = (unsigned char)(s->tpl[i].nfr & 255);
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    buf[n++] = (unsigned char)(s->tpl[i].nfr >> 8);
    size_t fb = (size_t)s->tpl[i].nfr * SP_NCEP * sizeof(float);
    nl_memcpy(buf + n, s->tpl[i].f, fb);
    n += fb;
  }

  char tmp[SP_PATH];
  nl_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  make_dirs(path);
  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    nl_free(buf);

    return -1;
  }

  size_t off = 0;
  int ok = 1;
  while (off < n) {
    ssize_t w = write(fd, buf + off, n - off);
    if (w <= 0) {
      ok = 0;
      break;
    }

    off += (size_t)w;
  }

  close(fd);
  nl_free(buf);
  if (!ok || rename(tmp, path) != 0) {
    unlink(tmp);

    return -1;
  }

  return 0;
}

int sp_load(sp_t *s, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return 0; // nothing enrolled yet
  }

  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  size_t cap = 1 << 16, n = 0;
  unsigned char *buf = nl_malloc(cap);
  if (!buf) {
    close(fd);

    return -1;
  }

  for (;;) {
    if (n == cap) {
      // NOLINTNEXTLINE(bugprone-signed-bitwise)
      if (cap >= SP_FILE_MAX) {
        close(fd);
        nl_free(buf);

        return -1;
      }

      unsigned char *nb = nl_malloc(cap * 2);
      if (!nb) {
        close(fd);
        nl_free(buf);

        return -1;
      }

      nl_memcpy(nb, buf, n);
      nl_free(buf);
      buf = nb;
      cap *= 2;
    }

    ssize_t r = read(fd, buf + n, cap - n);
    if (r < 0) {
      close(fd);
      nl_free(buf);

      return -1;
    }

    if (r == 0) {
      break;
    }

    n += (size_t)r;
  }

  close(fd);

  int rc = -1;
  if (n >= 12 && get32(buf) == SP_MAGIC && get32(buf + 4) == 1) {
    uint32_t count = get32(buf + 8);
    size_t p = 12;
    rc = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (p + 1 > n) {
        rc = -1;
        break;
      }

      size_t wl = buf[p++];
      if (wl == 0 || wl >= SP_WORD || p + wl + 2 > n) {
        rc = -1;
        break;
      }

      char word[SP_WORD];
      nl_memcpy(word, buf + p, wl);
      word[wl] = 0;
      p += wl;
      // NOLINTNEXTLINE(bugprone-signed-bitwise)
      int nfr = buf[p] | buf[p + 1] << 8;
      p += 2;
      size_t fb = (size_t)nfr * SP_NCEP * sizeof(float);
      if (nfr < SP_MIN_FRAMES || nfr > SP_MAXFR || p + fb > n) {
        rc = -1;
        break;
      }

      float *f = nl_malloc(fb);
      if (!f) {
        rc = -1;
        break;
      }

      nl_memcpy(f, buf + p, fb);
      p += fb;
      // a store that is full or has too many of a word keeps what it has
      int add = sp_add(s, word, f, nfr);
      nl_free(f);
      if (add != 0) {
        rc = -1;
        break;
      }
    }
  }

  nl_free(buf);

  return rc;
}

int sp_default_path(char *out, size_t size, const char *xdg, const char *home) {
  int n;
  if (xdg && *xdg) {
    n = nl_snprintf(out, size, "%s/asciicam/voice.tpl", xdg);
  } else if (home && *home) {
    n = nl_snprintf(out, size, "%s/.config/asciicam/voice.tpl", home);
  } else {
    return -1;
  }

  return n > 0 && (size_t)n + 1 < size ? 0 : -1;
}

/* Audio bytes */

enum { ST_START, ST_CHUNK, ST_FMT, ST_SKIP, ST_DATA };

void sp_stream_init(sp_stream_t *st) {
  nl_memset(st, 0, sizeof(*st));
  st->state = ST_START;
  st->rate = SP_RATE;
  st->chans = 1;
  st->bits = 16;
  st->ok = 1;
}

static const unsigned char riff[4] = {'R', 'I', 'F', 'F'};

static void st_fail(sp_stream_t *st, const char *why) {
  st->ok = 0;
  st->state = ST_SKIP;
  st->skip = ~0ul;
  nl_strncpy_safe(st->err, why, sizeof(st->err));
}

static void st_audio(sp_stream_t *st, sp_t *s, const unsigned char *d,
                     size_t n) {
  int16_t out[256];
  int no = 0;
  for (size_t i = 0; i < n; i++) {
    if (!st->odd) {
      st->lo = d[i];
      st->odd = 1;
      continue;
    }

    st->odd = 0;
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    int v = (int16_t)(st->lo | d[i] << 8);
    st->mix += v;
    if (++st->fi < st->chans) {
      continue;
    }

    int x = (int)(st->mix / st->chans);
    st->fi = 0;
    st->mix = 0;

    st->acc += x;
    st->cnt++;
    st->phase += SP_RATE;
    while (st->phase >= st->rate) {
      st->phase -= st->rate;
      if (st->cnt > 0) {
        st->last = (int)(st->acc / st->cnt);
        st->acc = 0;
        st->cnt = 0;
      }

      out[no++] = (int16_t)st->last;
      if (no == 256) {
        sp_feed(s, out, (size_t)no);
        no = 0;
      }
    }
  }

  if (no) {
    sp_feed(s, out, (size_t)no);
  }
}

static int tag_is(const unsigned char *p, const char *tag) {
  return p[0] == (unsigned char)tag[0] && p[1] == (unsigned char)tag[1] &&
         p[2] == (unsigned char)tag[2] && p[3] == (unsigned char)tag[3];
}

// NOLINTNEXTLINE(bugprone-signed-bitwise)
static int le16(const unsigned char *p) { return p[0] | p[1] << 8; }

void sp_stream_feed(sp_stream_t *st, sp_t *s, const unsigned char *data,
                    size_t n) {
  size_t i = 0;
  while (i < n) {
    switch (st->state) {
    case ST_START: {
      // 12 bytes tell RIFF/WAVE from raw samples
      while (i < n && st->nh < 12) {
        st->hdr[st->nh++] = data[i++];
        if (st->nh <= 4 && st->hdr[st->nh - 1] != riff[st->nh - 1]) {
          break;
        }
      }

      if (st->nh <= 4 && st->hdr[st->nh - 1] != riff[st->nh - 1]) {
        // not a WAV: what was collected is audio
        st->state = ST_DATA;
        unsigned char keep[12] = {0};
        int k = st->nh;
        nl_memcpy(keep, st->hdr, (size_t)k);
        st->nh = 0;
        st_audio(st, s, keep, (size_t)k);
      } else if (st->nh == 12) {
        if (tag_is(st->hdr + 8, "WAVE")) {
          st->state = ST_CHUNK;
          st->nh = 0;
        } else {
          st->state = ST_DATA;
          unsigned char keep[12] = {0};
          nl_memcpy(keep, st->hdr, 12);
          st->nh = 0;
          st_audio(st, s, keep, 12);
        }
      }

      break;
    }

    case ST_CHUNK:
      while (i < n && st->nh < 8) {
        st->hdr[st->nh++] = data[i++];
      }

      if (st->nh == 8) {
        unsigned long size = get32(st->hdr + 4);
        st->nh = 0;
        if (tag_is(st->hdr, "fmt ")) {
          if (size < 16 || size > 40) {
            st_fail(st, "bad fmt chunk");
          } else {
            // NOLINTNEXTLINE(bugprone-signed-bitwise)
            st->skip = size + (size & 1);
            st->state = ST_FMT;
          }
        } else if (tag_is(st->hdr, "data")) {
          if (st->bits != 16) {
            st_fail(st, "only 16 bit PCM is supported");
          } else if (st->chans < 1 || st->chans > 8 || st->rate < 8000 ||
                     st->rate > 192000) {
            st_fail(st, "unsupported channel count or rate");
          } else {
            st->state = ST_DATA;
          }
        } else {
          // NOLINTNEXTLINE(bugprone-signed-bitwise)
          st->skip = size + (size & 1);
          st->state = ST_SKIP;
        }
      }

      break;

    case ST_FMT:
      while (i < n && st->skip > 0) {
        if (st->nh < 16) {
          st->hdr[st->nh++] = data[i];
        }

        i++;
        st->skip--;
      }

      if (st->skip == 0) {
        int tag = le16(st->hdr);
        if (tag != 1 && tag != 0xfffe) {
          st_fail(st, "not PCM");
        } else {
          st->chans = le16(st->hdr + 2);
          st->rate = (int)get32(st->hdr + 4);
          st->bits = le16(st->hdr + 14);
          st->state = ST_CHUNK;
        }

        st->nh = 0;
      }

      break;

    case ST_SKIP: {
      size_t take = n - i;
      if ((unsigned long)take > st->skip) {
        take = st->skip;
      }

      i += take;
      st->skip -= take;
      if (st->skip == 0 && st->ok) {
        st->state = ST_CHUNK;
      }

      break;
    }

    default:
      st_audio(st, s, data + i, n - i);
      i = n;
      break;
    }
  }
}

/* Enrolling */

typedef struct {
  sp_t *store;
  const char *word;
  int added;
} enrol_t;

static void enrol_utt(void *user, const float *f, int n) {
  enrol_t *e = user;
  if (sp_add(e->store, e->word, f, n) == 0) {
    e->added++;
  }
}

int sp_enroll(sp_t *s, const char *word, const char *path, char *err,
              size_t err_size) {
  static sp_t rec; // a recogniser of its own, store stays untouched
  static sp_stream_t st;
  enrol_t e = {.store = s, .word = word, .added = 0};

  int fd = (path[0] == '-' && path[1] == 0) ? 0 : open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }

  sp_init(&rec);
  sp_on_utterance(&rec, enrol_utt, &e);
  sp_stream_init(&st);

  unsigned char tmp[4096] = {0};
  for (;;) {
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r < 0) {
      if (fd) {
        close(fd);
      }

      sp_on_utterance(&rec, NULL, NULL);

      return -1;
    }

    if (r == 0) {
      break;
    }

    sp_stream_feed(&st, &rec, tmp, (size_t)r);
  }

  if (fd) {
    close(fd);
  }

  sp_flush(&rec);
  sp_on_utterance(&rec, NULL, NULL); // e goes out of scope
  if (!st.ok) {
    if (err && err_size) {
      nl_strncpy_safe(err, st.err, err_size);
    }

    return -2;
  }

  sp_free(&rec);

  return e.added;
}

/* File or fifo as source */

int sp_src_open(sp_src_t *src, const char *path) {
  nl_memset(src, 0, sizeof(*src));
  src->fd = -1;
  sp_stream_init(&src->st);

  int fd;
  if (path[0] == '-' && path[1] == 0) {
    fd = 0;
  } else {
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    fd = open(path, O_RDWR | SP_O_NONBLOCK);
    if (fd < 0) {
      // NOLINTNEXTLINE(bugprone-signed-bitwise)
      fd = open(path, O_RDONLY | SP_O_NONBLOCK);
    }
  }

  if (fd < 0) {
    return -1;
  }

  src->fd = fd;
  if (fd == 0) {
    src->seekable = 1; // stdin: a pipe still ends when its writer does
    return 0;
  }

#ifdef __LINUX_NOLIBC__
  src->seekable = __sc3(SYS_lseek, fd, 0, 1) >= 0;
#else
  src->seekable = lseek(fd, 0, 1) >= 0;
#endif

  return 0;
}

long sp_src_poll(sp_src_t *src, sp_t *s) {
  if (src->fd < 0) {
    return 0;
  }

  long total = 0;
  unsigned char tmp[4096] = {0};
  for (int rounds = 0; rounds < 16; rounds++) {
    ssize_t r = read(src->fd, tmp, sizeof(tmp));
    if (r < 0) {
      break;
    }

    if (r == 0) {
      if (src->seekable && !src->eof) {
        src->eof = 1;
        sp_flush(s);
      }

      break;
    }

    sp_stream_feed(&src->st, s, tmp, (size_t)r);
    total += r;
  }

  return total;
}

void sp_src_close(sp_src_t *src) {
  if (src->fd > 0) {
    close(src->fd);
  }

  src->fd = -1;
}

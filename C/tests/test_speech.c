/*
 * tests/test_speech.c - speech recognition (src/speech.c)
 *
 * "speech" is synthesised: a buzz at a voice pitch whose harmonics are shaped
 * by two formants that move from segment to segment, so a word is a sequence of
 * vowel-like sounds with its own spectral shape in time
 * It can be said slower, faster and at another pitch, and then it has to be
 * same word
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_speech.c src/speech.c lib/nl_*.c \
 *     -Iinclude -Ilib -o /tmp/tsp
 */

#include "../lib/nolibc.h"
#include "platform.h"
#include "speech.h"

int memcmp(const void *a, const void *b, size_t n);

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

/* Synthesis */

#define PI 3.14159265358979

static double tsin(double x) {
  double k = x / (2 * PI);
  long n = (long)(k + (k < 0 ? -0.5 : 0.5));
   x -= (double)n * 2.0 * PI;
  double x2 = x * x, t = x, r = x;
  for (int i = 1; i < 12; i++) {
    t *= -x2 / ((2.0 * i) * (2.0 * i + 1));
    r += t;
  }

  return r;
}

static float sintab[8192];

typedef struct {
  int ms;
  int f1, f2; // formants, Hz
} seg_t;

static unsigned rng_state = 12345;
static int rnd(void) {
  rng_state = rng_state * 1664525u + 1013904223u;

  return (int)(rng_state >> 8);
}

// Write a word at `tempo` (1 = as is) and pitch f0; returns samples
static int render(int16_t *out, int max, const seg_t *seg, int nseg,
                  double tempo, double f0) {
  int n = 0;
  for (int s = 0; s < nseg; s++) {
    int len = (int)(seg[s].ms * tempo * SP_RATE / 1000.0);
    for (int i = 0; i < len && n < max; i++, n++) {
      double t = (double)n / SP_RATE;
      double v = 0;
      for (int h = 1; h * f0 < 4500; h++) {
        double f = h * f0;
        double a1 =
            1.0 / (1.0 + ((f - seg[s].f1) / (0.12 * seg[s].f1 + 60)) *
                             ((f - seg[s].f1) / (0.12 * seg[s].f1 + 60)));
        double a2 =
            0.7 / (1.0 + ((f - seg[s].f2) / (0.12 * seg[s].f2 + 60)) *
                             ((f - seg[s].f2) / (0.12 * seg[s].f2 + 60)));
        double ph = f * t;
        // ph = fmod(ph, 1.0); // TODO: Implement fmod in math.h
        ph -= (double)(long)ph;
        v += (a1 + a2) * sintab[(int)(ph * 8192) & 8191];
      }

      // Short fades at ends of a segment
      double env = 1.0;
      if (i < 80) {
        env = i / 80.0;
      } else if (len - i < 80) {
        env = (len - i) / 80.0;
      }

      out[n] = (int16_t)(v * env * 1500.0);
    }
  }

  return n;
}

static int silence(int16_t *out, int ms, int noise) {
  int len = ms * SP_RATE / 1000;
  for (int i = 0; i < len; i++) {
    out[i] = (int16_t)(noise ? (rnd() % (2 * noise + 1)) - noise : 0);
  }

  return len;
}

static const seg_t W_A[] = {
    {120, 300, 2200}, {150, 700, 1200}, {120, 400, 800}};
static const seg_t W_B[] = {
    {150, 600, 1000}, {120, 300, 2500}, {150, 500, 1500}};
static const seg_t W_C[] = {{100, 800, 1300}, {200, 350, 2000}, {90, 600, 900}};
static const seg_t W_D[] = {{200, 250, 900}, {100, 450, 3000}};
// not enrolled, and not close to any word that is
static const seg_t W_X[] = {
    {160, 900, 1700}, {160, 250, 3200}, {140, 750, 2400}};

typedef struct {
  const char *name;
  const seg_t *seg;
  int n;
} word_t;

static const word_t WORDS[] = {{"alpha", W_A, 3},
                               {"bravo", W_B, 3},
                               {"charlie", W_C, 3},
                               {"delta", W_D, 2}};

#define NW 4

static int16_t pcm[SP_RATE * 12];

static int say(int16_t *at, const word_t *w, double tempo, double f0) {
  return render(at, (int)(sizeof(pcm) / 2 - 1), w->seg, w->n, tempo, f0);
}

/* Collect what recogniser says */

typedef struct {
  int n;
  int final[32];
  char text[32][SP_WORD];
} heard_t;

static void hear(void *user, int final, const char *text) {
  heard_t *h = user;
  if (h->n < 32) {
    h->final[h->n] = final;
    nl_strncpy_safe(h->text[h->n], text, SP_WORD);
    h->n++;
  }
}

static int finals(const heard_t *h) {
  int n = 0;
  for (int i = 0; i < h->n; i++) {
    n += h->final[i];
  }

  return n;
}

// Text of k-th final, or ""
static const char *final_at(const heard_t *h, int k) {
  for (int i = 0; i < h->n; i++) {
    if (h->final[i] && k-- == 0) {
      return h->text[i];
    }
  }

  return "";
}

typedef struct {
  int n;
  int nfr[16];
} utts_t;

static void utt_cb(void *user, const float *f, int nfr) {
  (void)f;
  utts_t *u = user;
  if (u->n < 16) {
    u->nfr[u->n++] = nfr;
  }
}

static sp_t S;

// Enrol each word with three renditions
static void enrol_all(sp_t *sp) {
  static const double tempo[3] = {1.0, 0.85, 1.2};
  static const double pitch[3] = {120, 105, 135};
  for (int w = 0; w < NW; w++) {
    for (int r = 0; r < 3; r++) {
      int n = 0;
      n += silence(pcm + n, 300, 3);
      n += say(pcm + n, &WORDS[w], tempo[r], pitch[r]);
      n += silence(pcm + n, 500, 3);
      int fd = open("/tmp/asciicam_test_enrol.pcm",
                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
      write(fd, pcm, (size_t)n * 2);
      close(fd);
      int added =
          sp_enroll(sp, WORDS[w].name, "/tmp/asciicam_test_enrol.pcm", NULL, 0);
      CHECK(added == 1, "enrol %s rendition %d added %d", WORDS[w].name, r,
            added);
    }
  }

  unlink("/tmp/asciicam_test_enrol.pcm");
}

/* Tests */

static void test_segmentation(void) {
  sp_t *sp = &S;
  sp_init(sp);
  static utts_t u;
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);

  int n = 0;
  n += silence(pcm + n, 800, 3);
  n += say(pcm + n, &WORDS[0], 1.0, 120);
  n += silence(pcm + n, 700, 3);
  n += say(pcm + n, &WORDS[1], 1.0, 120);
  n += silence(pcm + n, 700, 3);
  sp_feed(sp, pcm, (size_t)n);
  sp_flush(sp);
  CHECK(u.n == 2, "two words in, %d utterances out", u.n);
  // 390 ms and 420 ms, plus few frames kept before and after
  if (u.n == 2) {
    CHECK(u.nfr[0] > 35 && u.nfr[0] < 55, "first is %d frames", u.nfr[0]);
    CHECK(u.nfr[1] > 38 && u.nfr[1] < 58, "second is %d frames", u.nfr[1]);
  }

  // Silence, stationary noise and a click are not utterances
  sp_init(sp);
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  n = silence(pcm, 3000, 0);
  sp_feed(sp, pcm, (size_t)n);
  sp_flush(sp);
  CHECK(u.n == 0, "digital silence is not speech (%d)", u.n);

  sp_init(sp);
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  n = silence(pcm, 4000, 300);
  sp_feed(sp, pcm, (size_t)n);
  sp_flush(sp);
  CHECK(u.n == 0, "steady noise is not speech (%d)", u.n);

  sp_init(sp);
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  n = 0;
  n += silence(pcm + n, 600, 20);
  for (int i = 0; i < 160; i++) { // a 10 ms click
    pcm[n++] = (int16_t)((i & 1) ? 12000 : -12000);
  }

  n += silence(pcm + n, 600, 20);
  sp_feed(sp, pcm, (size_t)n);
  sp_flush(sp);
  CHECK(u.n == 0, "a click is not a word (%d)", u.n);

  // A word that never stops is cut at SP_MAXFR, and listening goes on
  sp_init(sp);
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  n = silence(pcm, 400, 3);
  sp_feed(sp, pcm, (size_t)n);
  for (int k = 0; k < 6; k++) {
    n = say(pcm, &WORDS[0], 3.0, 120);
    sp_feed(sp, pcm, (size_t)n);
  }

  sp_flush(sp);
  CHECK(u.n >= 2, "long speech comes in pieces (%d: %d %d)", u.n, u.nfr[0],
        u.nfr[1]);
  for (int i = 0; i < u.n; i++) {
    CHECK(u.nfr[i] <= SP_MAXFR, "piece %d has %d frames", i, u.nfr[i]);
  }

  // A pause inside a phrase (150 ms) does not split it, a longer one does
  sp_init(sp);
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  n = silence(pcm, 500, 3);
  n += say(pcm + n, &WORDS[0], 1.0, 120);
  n += silence(pcm + n, 150, 3);
  n += say(pcm + n, &WORDS[1], 1.0, 120);
  n += silence(pcm + n, 600, 3);
  sp_feed(sp, pcm, (size_t)n);
  sp_flush(sp);
  CHECK(u.n == 1, "a 150 ms pause stays inside the utterance (%d)", u.n);

  // After a reset same signal gives same result
  sp_reset(sp);
  CHECK(sp->speaking == 0 && sp->nfr == 0, "reset leaves no utterance");
}

static void test_recognition(void) {
  sp_t *sp = &S;
  sp_init(sp);
  enrol_all(sp);
  CHECK(sp_words(sp) == NW, "words %d", sp_words(sp));
  CHECK(sp->ntpl == NW * 3, "templates %d", sp->ntpl);

  static heard_t h;
  static const double tempo[5] = {0.92, 1.1, 1.0, 0.8, 1.25};
  static const double pitch[5] = {112, 128, 100, 140, 120};
  int wrong = 0, missed = 0, total = 0;
  for (int w = 0; w < NW; w++) {
    for (int k = 0; k < 5; k++) {
      sp_reset(sp);
      h.n = 0;
      sp_set_text(sp, hear, &h);
      int n = silence(pcm, 500, 3);
      n += say(pcm + n, &WORDS[w], tempo[k], pitch[k]);
      n += silence(pcm + n, 600, 3);
      sp_feed(sp, pcm, (size_t)n);
      total++;
      if (finals(&h) == 0) {
        missed++;
        fprintf(stderr,
                "  missed %s tempo %d%% pitch %d: d=%d.%02d m=%d.%02d\n",
                WORDS[w].name, (int)(tempo[k] * 100), (int)pitch[k],
                (int)sp->last_dist, (int)(sp->last_dist * 100) % 100,
                (int)sp->last_margin, (int)(sp->last_margin * 100) % 100);
      } else if (finals(&h) != 1 || nl_strcmp(final_at(&h, 0), WORDS[w].name)) {
        wrong++;
        fprintf(stderr, "  %s heard as '%s' (%d finals)\n", WORDS[w].name,
                final_at(&h, 0), finals(&h));
      }
    }
  }

  CHECK(wrong == 0, "%d of %d said as another word", wrong, total);
  CHECK(missed == 0, "%d of %d not recognised", missed, total);

  // A word that is not enrolled, and noise, must not turn into one that is
  sp_reset(sp);
  h.n = 0;
  int utt_before = sp->utterances;
  int n = silence(pcm, 500, 3);
  n += render(pcm + n, 40000, W_X, 3, 1.0, 120);
  n += silence(pcm + n, 600, 3);
  sp_feed(sp, pcm, (size_t)n);
  CHECK(finals(&h) == 0, "unknown word heard as '%s'", final_at(&h, 0));
  CHECK(sp->utterances == utt_before + 1, "it was still an utterance (%d)",
        sp->utterances - utt_before);

  sp_reset(sp);
  h.n = 0;
  n = silence(pcm, 500, 3);
  for (int i = 0; i < 6400; i++) { // 400 ms burst of loud noise
    pcm[n++] = (int16_t)((rnd() % 16001) - 8000);
  }

  n += silence(pcm + n, 600, 3);
  sp_feed(sp, pcm, (size_t)n);
  CHECK(finals(&h) == 0, "noise burst heard as '%s'", final_at(&h, 0));

  // With nothing to compare against, only threshold can refuse: one word
  // enrolled, and a different word said
  static sp_t one;
  sp_init(&one);
  sp_enroll(&one, "alpha", "/tmp/asciicam_test_one.pcm", NULL, 0); // no file
  for (int i = 0; i < 3; i++) {
    int m = silence(pcm, 300, 3);
    m += say(pcm + m, &WORDS[0], 1.0 - 0.1 * i, 120 + 10 * i);
    m += silence(pcm + m, 500, 3);
    int fd1 =
        open("/tmp/asciicam_test_one.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd1, pcm, (size_t)m * 2);
    close(fd1);
    sp_enroll(&one, "alpha", "/tmp/asciicam_test_one.pcm", NULL, 0);
  }

  unlink("/tmp/asciicam_test_one.pcm");
  CHECK(one.ntpl == 3, "single word store has %d templates", one.ntpl);
  static heard_t oh;
  oh.n = 0;
  sp_set_text(&one, hear, &oh);
  n = silence(pcm, 500, 3);
  n += render(pcm + n, 40000, W_X, 3, 1.0, 120);
  n += silence(pcm + n, 600, 3);
  sp_feed(&one, pcm, (size_t)n);
  CHECK(finals(&oh) == 0, "a stranger to a one word store is refused (d=%d)",
        (int)(one.last_dist * 100));
  oh.n = 0;
  n = silence(pcm, 500, 3);
  n += say(pcm + n, &WORDS[0], 1.05, 118);
  n += silence(pcm + n, 600, 3);
  sp_feed(&one, pcm, (size_t)n);
  CHECK(finals(&oh) == 1, "and the word itself is taken");
  sp_free(&one);

  // same word again and again: all of them, each once
  sp_reset(sp);
  h.n = 0;
  n = 0;
  for (int k = 0; k < 4; k++) {
    n += silence(pcm + n, 500, 3);
    n += say(pcm + n, &WORDS[2], 1.0, 118);
  }

  n += silence(pcm + n, 600, 3);
  sp_feed(sp, pcm, (size_t)n);
  CHECK(finals(&h) == 4, "four words said, %d heard", finals(&h));

  // A hypothesis comes before final, and is right word
  sp_reset(sp);
  h.n = 0;
  n = silence(pcm, 500, 3);
  n += say(pcm + n, &WORDS[1], 2.0, 120); // slow: 0.84 s
  n += silence(pcm + n, 600, 3);
  sp_feed(sp, pcm, (size_t)n);
  int partials = 0, bad = 0;
  for (int i = 0; i < h.n; i++) {
    if (!h.final[i]) {
      partials++;
      bad += nl_strcmp(h.text[i], "bravo") != 0;
    }
  }

  CHECK(finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "bravo") == 0,
        "slow word final '%s'", final_at(&h, 0));
  CHECK(partials >= 1, "no hypothesis while it was being said");
  CHECK(bad == 0, "%d wrong hypotheses", bad);
  if (h.n > 1) {
    CHECK(h.final[h.n - 1] == 1, "the final comes last");
  }

  // Two words enrolled with same sound are an ambiguity, not an answer
  static sp_t twin;
  sp_init(&twin);
  int fd =
      open("/tmp/asciicam_test_enrol.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  n = silence(pcm, 300, 3);
  n += say(pcm + n, &WORDS[0], 1.0, 120);
  n += silence(pcm + n, 500, 3);
  write(fd, pcm, (size_t)n * 2);
  close(fd);
  CHECK(sp_enroll(&twin, "one", "/tmp/asciicam_test_enrol.pcm", NULL, 0) == 1,
        "enrol one");
  CHECK(sp_enroll(&twin, "uno", "/tmp/asciicam_test_enrol.pcm", NULL, 0) == 1,
        "enrol uno");
  unlink("/tmp/asciicam_test_enrol.pcm");
  static heard_t th;
  sp_set_text(&twin, hear, &th);
  sp_feed(&twin, pcm, (size_t)n);
  CHECK(finals(&th) == 0, "a tie is answered with '%s'", final_at(&th, 0));
  sp_free(&twin);

  sp_free(sp);
}

static void test_dtw(void) {
  static float a[60 * SP_NCEP], b[90 * SP_NCEP];
  for (int i = 0; i < 60; i++) {
    for (int k = 0; k < SP_NCEP; k++) {
      a[i * SP_NCEP + k] = (float)((i * (k + 1)) % 17) - 8.0f;
    }
  }

  // b is a stretched by 1.5
  for (int i = 0; i < 90; i++) {
    for (int k = 0; k < SP_NCEP; k++) {
      b[i * SP_NCEP + k] = a[(i * 2 / 3) * SP_NCEP + k];
    }
  }

  float d_same = sp_dtw(a, 60, a, 60, 0);
  float d_warp = sp_dtw(a, 60, b, 90, 0);
  float d_back = sp_dtw(b, 90, a, 60, 0);
  CHECK(d_same == 0.0f, "same utterance is distance 0 (%d)",
        (int)(d_same * 100));
  CHECK(d_warp < 1.0f, "a stretched copy is close (%d)", (int)(d_warp * 100));
  CHECK(d_warp - d_back < 0.01f && d_back - d_warp < 0.01f,
        "symmetric: %d vs %d", (int)(d_warp * 1000), (int)(d_back * 1000));

  // Something else is far
  static float c[60 * SP_NCEP];
  for (int i = 0; i < 60 * SP_NCEP; i++) {
    c[i] = (float)((i * 7) % 13) - 6.0f;
  }

  CHECK(sp_dtw(a, 60, c, 60, 0) > 2.0f, "different is far");

  // Lengths more than 2:1 apart can not be same word
  CHECK(sp_dtw(a, 20, a, 60, 0) >= 1e29f, "3:1 is refused");
  CHECK(sp_dtw(a, 0, a, 60, 0) >= 1e29f, "empty is refused");
  CHECK(sp_dtw(a, 60, a, SP_MAXFR + 1, 0) >= 1e29f, "too long is refused");

  // Open end: first 30 frames of a match a, but do not match it whole
  float d_prefix = sp_dtw(a, 30, a, 60, 1);
  float d_whole = sp_dtw(a, 30, a, 60, 0);
  CHECK(d_prefix < 0.5f, "a prefix matches open ended (%d)",
        (int)(d_prefix * 100));
  CHECK(d_whole > d_prefix, "and not closed");
}

static void test_store(void) {
  static sp_t sp, back;
  sp_init(&sp);
  static float f[40 * SP_NCEP];
  for (int i = 0; i < 40 * SP_NCEP; i++) {
    f[i] = (float)(i % 11) * 0.5f;
  }

  CHECK(sp_add(&sp, "yes", f, 40) == 0, "add");
  CHECK(sp_add(&sp, "", f, 40) == -1, "empty word");
  CHECK(sp_add(&sp, NULL, f, 40) == -1, "NULL word");
  CHECK(sp_add(&sp, "x", f, 9) == -1, "too short");
  CHECK(sp_add(&sp, "x", f, SP_MAXFR + 1) == -1, "too long");
  char longword[SP_WORD + 8];
  nl_memset(longword, 'w', sizeof(longword));
  longword[sizeof(longword) - 1] = 0;
  CHECK(sp_add(&sp, longword, f, 40) == -1, "word too long");
  CHECK(sp_count(&sp, "yes") == 1 && sp_words(&sp) == 1, "counts");

  // 6th template of a word pushes out first
  for (int k = 1; k <= SP_PER_WORD; k++) {
    for (int i = 0; i < 40 * SP_NCEP; i++) {
      f[i] = (float)k;
    }

    sp_add(&sp, "yes", f, 40);
  }

  CHECK(sp_count(&sp, "yes") == SP_PER_WORD, "at most %d (%d)", SP_PER_WORD,
        sp_count(&sp, "yes"));
  int oldest_gone = 1;
  for (int i = 0; i < sp.ntpl; i++) {
    if (sp.tpl[i].f[0] == 0.0f) {
      oldest_gone = 0; // very first had 0.0 in f[0]
    }
  }

  CHECK(oldest_gone, "the oldest went");
  CHECK(sp.tpl[sp.ntpl - 1].f[0] == (float)SP_PER_WORD, "the newest is last");

  // Fill store: next is refused, not a crash
  for (int w = 0; sp.ntpl < SP_MAX_TPL; w++) {
    char name[16];
    nl_snprintf(name, sizeof(name), "w%d", w);
    CHECK(sp_add(&sp, name, f, 40) == 0, "fill %d", w);
  }

  CHECK(sp_add(&sp, "overflow", f, 40) == -1, "full store refuses");
  CHECK(sp.ntpl == SP_MAX_TPL, "and stays full");

  // Remove
  CHECK(sp_remove(&sp, "yes") == SP_PER_WORD, "removed all of yes");
  CHECK(sp_remove(&sp, "yes") == 0, "twice");
  CHECK(sp_count(&sp, "yes") == 0, "gone");

  // Save and load
  sp_free(&sp);
  sp_init(&sp);
  static float g[55 * SP_NCEP];
  for (int i = 0; i < 55 * SP_NCEP; i++) {
    g[i] = (float)i * 0.25f - 3.0f;
  }

  sp_add(&sp, "good morning", g, 55);
  sp_add(&sp, "yes", f, 40);
  sp_add(&sp, "yes", g, 55);
  const char *path = "/tmp/asciicam_test_voice/a/b/voice.tpl";
  CHECK(sp_save(&sp, path) == 0, "save creates directories");

  sp_init(&back);
  CHECK(sp_load(&back, path) == 0, "load");
  CHECK(back.ntpl == 3, "three templates back (%d)", back.ntpl);
  int same = back.ntpl == 3;
  for (int i = 0; same && i < 3; i++) {
    same = nl_strcmp(back.tpl[i].word, sp.tpl[i].word) == 0 &&
           back.tpl[i].nfr == sp.tpl[i].nfr &&
           memcmp(back.tpl[i].f, sp.tpl[i].f,
                  (size_t)sp.tpl[i].nfr * SP_NCEP * sizeof(float)) == 0;
  }

  CHECK(same, "same words, lengths and numbers");
  sp_free(&back);

  // Loading again adds, up to per word limit
  sp_init(&back);
  sp_load(&back, path);
  sp_load(&back, path);
  CHECK(sp_count(&back, "yes") == 4, "two loads: yes x%d",
        sp_count(&back, "yes"));
  sp_free(&back);

  // A file that is not there is an empty store, a damaged one is an error
  sp_init(&back);
  CHECK(sp_load(&back, "/tmp/asciicam_test_voice/none.tpl") == 0 &&
            back.ntpl == 0,
        "missing file");

  int fd = open(path, O_RDONLY);
  static unsigned char raw[8192];
  ssize_t rn = read(fd, raw, sizeof(raw));
  close(fd);
  CHECK(rn > 100, "file is %d bytes", (int)rn);

  const char *bad = "/tmp/asciicam_test_voice/bad.tpl";
  int cuts[] = {0, 3, 11, 12, 14, 30, (int)rn / 2, (int)rn - 1};
  for (unsigned c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
    fd = open(bad, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, raw, (size_t)cuts[c]);
    close(fd);
    sp_init(&back);
    int rc = sp_load(&back, bad);
    CHECK(rc == -1, "cut at %d: %d", cuts[c], rc);
    sp_free(&back);
  }

  // Wrong magic, wrong version, a word of length 0, a frame count of 0
  unsigned char mod[8192];
  struct {
    int at;
    unsigned char v;
  } hits[] = {{0, 'X'}, {4, 9}, {12, 0}, {12 + 1 + 12, 0}};
  for (unsigned c = 0; c < sizeof(hits) / sizeof(hits[0]); c++) {
    nl_memcpy(mod, raw, (size_t)rn);
    mod[hits[c].at] = hits[c].v;
    if (c == 3) {
      mod[hits[c].at + 1] = 0;
    }

    fd = open(bad, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, mod, (size_t)rn);
    close(fd);
    sp_init(&back);
    CHECK(sp_load(&back, bad) == -1, "damage %u", c);
    sp_free(&back);
  }

  // Random bytes
  for (int round = 0; round < 40; round++) {
    for (int i = 0; i < 400; i++) {
      mod[i] = (unsigned char)rnd();
    }

    if (round & 1) {
      mod[0] = 'A';
      mod[1] = 'S';
      mod[2] = 'P';
      mod[3] = 'T';
      mod[4] = 1;
      mod[5] = mod[6] = mod[7] = 0;
    }

    fd = open(bad, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, mod, 400);
    close(fd);
    sp_init(&back);
    sp_load(&back, bad); // must not crash
    sp_free(&back);
  }

  unlink(bad);
  unlink(path);
  sp_free(&sp);

  // Paths
  char p[64];
  CHECK(sp_default_path(p, sizeof(p), "/x/cfg", "/home/u") == 0 &&
            nl_strcmp(p, "/x/cfg/asciicam/voice.tpl") == 0,
        "xdg: %s", p);
  CHECK(sp_default_path(p, sizeof(p), NULL, "/home/u") == 0 &&
            nl_strcmp(p, "/home/u/.config/asciicam/voice.tpl") == 0,
        "home: %s", p);
  CHECK(sp_default_path(p, sizeof(p), "", "/home/u") == 0 &&
            nl_strcmp(p, "/home/u/.config/asciicam/voice.tpl") == 0,
        "empty xdg falls back to home");
  CHECK(sp_default_path(p, sizeof(p), NULL, NULL) == -1, "no home");
  CHECK(sp_default_path(p, 20, "/x/cfg", "/home/u") == -1, "does not fit");
  CHECK(sp_default_path(p, 26, "/x/cfg", "/home/u") == 0 ||
            sp_default_path(p, 26, "/x/cfg", "/home/u") == -1,
        "tight");
  CHECK(sp_default_path(p, 25, "/x/cfg", "/home/u") == -1, "one short");
}

/* Bytes: raw, WAV, odd splits */

static void put16(unsigned char *p, int v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned v) {
  put16(p, (int)(v & 0xffff));
  put16(p + 2, (int)(v >> 16));
}

// A WAV with a LIST chunk (odd sized, so padded) before fmt and data
static int make_wav(unsigned char *out, const int16_t *s, int n, int rate,
                    int chans, int bits, int tag) {
  int o = 0;
  nl_memcpy(out + o, "RIFF", 4);
  put32(out + o + 4, 0xffffffffu); // a stream does not know its length
  nl_memcpy(out + o + 8, "WAVE", 4);
  o += 12;
  nl_memcpy(out + o, "LIST", 4);
  put32(out + o + 4, 5);
  nl_memcpy(out + o + 8, "abcde\0", 6); // 5 bytes and 1 of padding
  o += 8 + 6;
  nl_memcpy(out + o, "fmt ", 4);
  put32(out + o + 4, 16);
  put16(out + o + 8, tag);
  put16(out + o + 10, chans);
  put32(out + o + 12, (unsigned)rate);
  put32(out + o + 16, (unsigned)(rate * chans * bits / 8));
  put16(out + o + 20, chans * bits / 8);
  put16(out + o + 22, bits);
  o += 24;
  nl_memcpy(out + o, "data", 4);
  put32(out + o + 4, 0);
  o += 8;
  // upsample by repeating, to `rate`, duplicated over channels
  long phase = 0;
  for (int i = 0; i < n;) {
    for (int c = 0; c < chans; c++) {
      put16(out + o, s[i]);
      o += 2;
    }

    phase += SP_RATE;
    while (phase >= rate) {
      phase -= rate;
      i++;
    }
  }

  return o;
}

static unsigned char wavbuf[1 << 21];

static void test_stream(void) {
  sp_t *sp = &S;
  sp_init(sp);
  enrol_all(sp);

  int n = silence(pcm, 500, 3);
  n += say(pcm + n, &WORDS[3], 1.0, 122);
  n += silence(pcm + n, 600, 3);

  static heard_t h;
  static sp_stream_t st;

  // Raw, all at once and one byte at a time must be same
  sp_reset(sp);
  h.n = 0;
  sp_set_text(sp, hear, &h);
  sp_stream_init(&st);
  sp_stream_feed(&st, sp, (const unsigned char *)pcm, (size_t)n * 2);
  CHECK(st.ok, "raw ok");
  CHECK(finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "delta") == 0,
        "raw: '%s'", final_at(&h, 0));

  sp_reset(sp);
  h.n = 0;
  sp_stream_init(&st);
  for (int i = 0; i < n * 2; i++) {
    sp_stream_feed(&st, sp, (const unsigned char *)pcm + i, 1);
  }

  CHECK(finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "delta") == 0,
        "byte by byte: '%s'", final_at(&h, 0));

  // WAV at 16 kHz mono, in awkward pieces, with a chunk to skip
  int wn = make_wav(wavbuf, pcm, n, 16000, 1, 16, 1);
  int sizes[] = {1, 2, 3, 5, 7, 13, 64, 1000};
  for (unsigned c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++) {
    sp_reset(sp);
    h.n = 0;
    sp_stream_init(&st);
    for (int i = 0; i < wn; i += sizes[c]) {
      int k = wn - i < sizes[c] ? wn - i : sizes[c];
      sp_stream_feed(&st, sp, wavbuf + i, (size_t)k);
    }

    CHECK(st.ok && finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "delta") == 0,
          "wav in pieces of %d: ok %d heard '%s'", sizes[c], st.ok,
          final_at(&h, 0));
  }

  // Stereo at 48 kHz (a whole number of 16 kHz samples per sample): same words
  // come out, though resampling is a cheap one
  wn = make_wav(wavbuf, pcm, n, 48000, 2, 16, 1);
  sp_reset(sp);
  h.n = 0;
  sp_stream_init(&st);
  sp_stream_feed(&st, sp, wavbuf, (size_t)wn);
  CHECK(st.ok && st.rate == 48000 && st.chans == 2, "fmt read: %d Hz %d ch",
        st.rate, st.chans);
  CHECK(finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "delta") == 0,
        "48 kHz stereo: '%s'", final_at(&h, 0));

  // Samples come out at 16 kHz: count them
  sp_reset(sp);
  sp_stream_init(&st);
  utts_t u;
  u.n = 0;
  sp_on_utterance(sp, utt_cb, &u);
  sp_stream_feed(&st, sp, wavbuf, (size_t)wn);
  sp_flush(sp);
  CHECK(u.n == 1 && u.nfr[0] > 25 && u.nfr[0] < 45, "48k utterance %d frames",
        u.n ? u.nfr[0] : -1);
  sp_on_utterance(sp, NULL, NULL);

  // Formats that can not be used say so and do not feed anything
  struct {
    int rate, chans, bits, tag;
    const char *what;
  } bad[] = {{16000, 1, 8, 1, "8 bit"},       {16000, 1, 24, 1, "24 bit"},
             {16000, 1, 16, 3, "float"},      {16000, 0, 16, 1, "no channels"},
             {16000, 9, 16, 1, "9 channels"}, {1000, 1, 16, 1, "1 kHz"},
             {400000, 1, 16, 1, "400 kHz"}};
  for (unsigned c = 0; c < sizeof(bad) / sizeof(bad[0]); c++) {
    wn = make_wav(wavbuf, pcm, 3200, bad[c].rate,
                  bad[c].chans ? bad[c].chans : 1, bad[c].bits, bad[c].tag);
    if (bad[c].chans == 0) {
      put16(wavbuf + 12 + 14 + 8 + 2, 0);
    }

    sp_reset(sp);
    h.n = 0;
    sp_stream_init(&st);
    sp_stream_feed(&st, sp, wavbuf, (size_t)wn);
    CHECK(!st.ok && st.err[0], "%s refused (%s)", bad[c].what, st.err);
    CHECK(st.state != 4 || !st.ok, "and nothing is read from it");
  }

  // Audio that only looks a little like a header is audio
  static unsigned char rf[64];
  nl_memcpy(rf, "RIFX", 4);
  for (int i = 4; i < 64; i++) {
    rf[i] = (unsigned char)i;
  }

  sp_reset(sp);
  sp_stream_init(&st);
  sp_stream_feed(&st, sp, rf, 64);
  CHECK(st.ok, "RIFX is not a WAV, and not an error");
  nl_memcpy(rf, "RIFF", 4);
  nl_memcpy(rf + 8, "AVI ", 4);
  sp_stream_init(&st);
  sp_stream_feed(&st, sp, rf, 64);
  CHECK(st.ok, "RIFF AVI is raw audio too");

  // Cut anywhere in a header and then more data: no crash
  wn = make_wav(wavbuf, pcm, 1600, 16000, 1, 16, 1);
  for (int cut = 0; cut < 60; cut++) {
    sp_reset(sp);
    sp_stream_init(&st);
    sp_stream_feed(&st, sp, wavbuf, (size_t)cut);
    sp_stream_feed(&st, sp, wavbuf + cut, (size_t)(wn - cut));
  }

  CHECK(1, "headers cut at every byte");

  // Rubbish in every position
  for (int round = 0; round < 300; round++) {
    wn = make_wav(wavbuf, pcm, 800, 16000, 1, 16, 1);
    int at = rnd() % 60;
    wavbuf[at] = (unsigned char)rnd();
    if (round & 1) {
      wavbuf[(at + 7) % 60] = (unsigned char)rnd();
    }

    sp_reset(sp);
    sp_stream_init(&st);
    sp_stream_feed(&st, sp, wavbuf, (size_t)wn);
  }

  CHECK(1, "damaged headers");

  // Huge chunk sizes are skipped, not allocated
  nl_memset(wavbuf, 0, 100);
  nl_memcpy(wavbuf, "RIFF", 4);
  nl_memcpy(wavbuf + 8, "WAVE", 4);
  nl_memcpy(wavbuf + 12, "JUNK", 4);
  put32(wavbuf + 16, 0xfffffff0u);
  sp_reset(sp);
  sp_stream_init(&st);
  sp_stream_feed(&st, sp, wavbuf, 100);
  CHECK(st.ok, "a 4 GB chunk is skipped over");

  sp_free(sp);
}

/* A file as source */

static void test_src(void) {
  static sp_t sp;
  static sp_src_t src;
  static heard_t h;
  sp_init(&sp);
  enrol_all(&sp);
  sp_set_text(&sp, hear, &h);

  int n = silence(pcm, 500, 3);
  n += say(pcm + n, &WORDS[0], 1.0, 120);
  n += silence(pcm + n, 100, 3); // ends before pause is over
  int fd =
      open("/tmp/asciicam_test_src.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  write(fd, pcm, (size_t)n * 2);
  close(fd);

  CHECK(sp_src_open(&src, "/tmp/does/not/exist") == -1, "missing file");
  CHECK(sp_src_open(&src, "/tmp/asciicam_test_src.pcm") == 0, "open");
  CHECK(src.seekable, "a file is seekable");
  long total = 0, got;
  while (!src.eof) {
    got = sp_src_poll(&src, &sp);
    total += got;
  }

  CHECK(total == (long)n * 2, "read %ld of %d bytes", total, n * 2);
  // End of file ends word as well
  CHECK(finals(&h) == 1 && nl_strcmp(final_at(&h, 0), "alpha") == 0,
        "word cut by the end of file: '%s'", final_at(&h, 0));
  CHECK(sp_src_poll(&src, &sp) == 0, "nothing more");
  sp_src_close(&src);
  CHECK(src.fd == -1, "closed");
  CHECK(sp_src_poll(&src, &sp) == 0, "closed source reads nothing");
  unlink("/tmp/asciicam_test_src.pcm");

  // An empty file is not an error, it just ends
  fd = open("/tmp/asciicam_test_src.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  close(fd);
  CHECK(sp_src_open(&src, "/tmp/asciicam_test_src.pcm") == 0, "open empty");
  sp_src_poll(&src, &sp);
  CHECK(src.eof, "empty file is at its end");
  sp_src_close(&src);
  unlink("/tmp/asciicam_test_src.pcm");

  // Enrol from a file that is not there, and that is not audio
  CHECK(sp_enroll(&sp, "x", "/tmp/does/not/exist", NULL, 0) == -1, "no file");
  fd = open("/tmp/asciicam_test_src.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  n = make_wav(wavbuf, pcm, 1600, 16000, 1, 8, 1);
  write(fd, wavbuf, (size_t)n);
  close(fd);
  char err[64] = {0};
  CHECK(sp_enroll(&sp, "x", "/tmp/asciicam_test_src.pcm", err, sizeof(err)) ==
            -2,
        "8 bit wav");
  CHECK(err[0] != 0, "says why: %s", err);
  unlink("/tmp/asciicam_test_src.pcm");
  CHECK(sp_count(&sp, "x") == 0, "nothing was added");

  // Silence is not a word
  fd = open("/tmp/asciicam_test_src.pcm", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  n = silence(pcm, 2000, 2);
  write(fd, pcm, (size_t)n * 2);
  close(fd);
  CHECK(sp_enroll(&sp, "x", "/tmp/asciicam_test_src.pcm", NULL, 0) == 0,
        "enrolling silence adds nothing");
  unlink("/tmp/asciicam_test_src.pcm");

  sp_free(&sp);
}

int main(void) {
  for (int i = 0; i < 8192; i++) {
    sintab[i] = (float)tsin(2 * PI * i / 8192);
  }

  test_dtw();
  test_segmentation();
  test_recognition();
  test_store();
  test_stream();
  test_src();

  fprintf(stderr, "speech: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

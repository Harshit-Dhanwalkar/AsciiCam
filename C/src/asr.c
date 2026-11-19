#include "asr.h"
#include "nolibc.h"
#include "safetensors.h"

void asr_init(asr_t *a) {
  nl_memset(a, 0, sizeof(*a));
  a->floor = -1.0f;
}

static void tap(void *user, const int16_t *pcm, size_t n) {
  asr_t *a = user;
  if (!pcm) {
    asr_flush(a);
  } else {
    asr_feed(a, pcm, n);
  }
}

void asr_attach(asr_t *a, sp_t *sp) { sp_on_pcm(sp, tap, a); }

void asr_set_text(asr_t *a, asr_text_fn fn, void *user) {
  a->text = fn;
  a->text_user = user;
}

int asr_use(asr_t *a, const an_model_t *model, const tokdec_t *tok) {
  if (a->stream) {
    an_stream_free(a->stream);
    a->stream = NULL;
  }

  a->model = model;
  a->tok = tok;
  a->stream = an_stream_new(model);

  return a->stream ? 0 : -1;
}

// "dir/model.safetensors" -> "dir/tokenizer.json"
static int sibling(char *out, size_t size, const char *path, const char *name) {
  size_t slash = 0;
  for (size_t i = 0; path[i]; i++) {
    if (path[i] == '/') {
      slash = i + 1;
    }
  }

  if (slash + nl_strlen(name) + 1 >= size) {
    return -1;
  }

  for (size_t i = 0; i < slash; i++) {
    out[i] = path[i];
  }

  size_t k = 0;
  while (name[k]) {
    out[slash + k] = name[k];
    k++;
  }

  out[slash + k] = 0;

  return 0;
}

int asr_open(asr_t *a, const char *model_path, const char *tokenizer_path,
             char *err, size_t err_size) {
  char tokpath[512];
  if (!tokenizer_path) {
    if (sibling(tokpath, sizeof(tokpath), model_path, "tokenizer.json") != 0) {
      nl_strncpy_safe(err, "model path too long", err_size);

      return -1;
    }

    tokenizer_path = tokpath;
  }

  size_t n = 0;
  unsigned char *json = st_read_file(tokenizer_path, &n, err, err_size);
  if (!json) {
    return -1;
  }

  int rc = tokdec_parse(&a->own_tok, (const char *)json, n, err, err_size);
  nl_free(json);
  if (rc != 0) {
    return -1;
  }

  // config.json beside weights gives shape, else tiny model's
  an_config_t cfg;
  const an_config_t *use = NULL;
  char cfgpath[512];
  if (sibling(cfgpath, sizeof(cfgpath), model_path, "config.json") == 0) {
    size_t cn = 0;
    char e2[96];
    unsigned char *cj = st_read_file(cfgpath, &cn, e2, sizeof(e2));
    if (cj) {
      int pr = an_config_parse(&cfg, (const char *)cj, cn, err, err_size);
      nl_free(cj);
      if (pr != 0) {
        tokdec_free(&a->own_tok);

        return -1;
      }

      use = &cfg;
    }
  }

  if (an_load(&a->own_model, model_path, use, err, err_size) != 0) {
    tokdec_free(&a->own_tok);

    return -1;
  }

  a->owns = 1;
  if (asr_use(a, &a->own_model, &a->own_tok) != 0) {
    nl_strncpy_safe(err, "out of memory", err_size);
    asr_close(a);

    return -1;
  }

  return 0;
}

void asr_close(asr_t *a) {
  if (a->stream) {
    an_stream_free(a->stream);
    a->stream = NULL;
  }

  if (a->owns) {
    an_free(&a->own_model);
    tokdec_free(&a->own_tok);
    a->owns = 0;
  }

  a->model = NULL;
  a->tok = NULL;
}

/* Reading what model heard */

static void say(asr_t *a, int final, const char *text) {
  if (a->text && text[0]) {
    a->text(a->text_user, final, text);
  }
}

// Text of utterance so far
int asr_trim_repeats(const int *in, int n, int *out, int short_max) {
  int w = 0;
  int i = 0;
  while (i < n) {
    int cut = 0;
    for (int p = 1; p <= ASR_REPEAT_LEN && !cut; p++) {
      int max = p <= 2 ? short_max : ASR_REPEAT_MAX;

      // Copies of block of length p that starts at i
      int copies = 1;
      while (i + (copies + 1) * p <= n) {
        int same = 1;
        for (int k = 0; k < p && same; k++) {
          same = in[i + k] == in[i + copies * p + k];
        }

        if (!same) {
          break;
        }

        copies++;
      }

      if (copies > max) {
        for (int c = 0; c < max * p; c++) {
          out[w++] = in[i + c];
        }

        i += copies * p;
        cut = 1;
      }
    }

    if (!cut) {
      out[w++] = in[i++];
    }
  }

  return w;
}

// prev (last call's pre-trim ids, same utterance) already ended in 2+ copies of
// a 1-2 id block and every id since is just that same block continuing with
// nothing new mixed in: model has been stuck on it for a whole ASR_PARTIAL_MS
// without saying anything else, which real speech essentially never does - a
// burst of repeated words still moves on to other words inside one partial
// window
int asr_still_looping(const int *prev, int pn, const int *cur, int cn) {
  if (cn <= pn) {
    return 0;
  }

  for (int p = 1; p <= 2; p++) {
    if (pn < 2 * p) {
      continue;
    }

    int same = 1;
    for (int k = 0; k < p && same; k++) {
      same = prev[pn - 1 - k] == prev[pn - 1 - p - k];
    }

    if (!same) {
      continue;
    }

    int ok = 1;
    for (int i = pn; i < cn && ok; i++) {
      ok = cur[i] == prev[pn - p + (i - pn) % p];
    }

    if (ok) {
      return 1;
    }
  }

  return 0;
}

static void read_text(asr_t *a, char *out, size_t size) {
  int toks[256];
  int kept[256];
  int n = an_decode(a->stream, toks, 256);
  if (n < 0) {
    n = 0;
  }

  // Only ids that print count as repeats: a control token between two words
  // must not hide a loop of them
  int shown[256] = {0};
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (tokdec_is_text(a->tok, toks[i])) {
      shown[m++] = toks[i];
    }
  }

  if (a->debug) {
    fprintf(stderr, "[asr-debug] decoded %d raw, %d shown:", n, m);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, " %d", toks[i]);
    }

    fprintf(stderr, "\n");
  }

  int short_max = asr_still_looping(a->prev_shown, a->prev_shown_n, shown, m)
                      ? ASR_REPEAT_MAX
                      : ASR_SHORT_REPEAT_MAX;
  n = asr_trim_repeats(shown, m, kept, short_max);

  if (a->debug) {
    fprintf(stderr, "[asr-debug] trimmed %d -> %d (short_max %d):", m, n,
            short_max);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, " %d", kept[i]);
    }

    fprintf(stderr, "\n");
  }

  // shown[] holds at most n ids and n is capped at 256 by an_decode
  nl_memcpy(a->prev_shown, shown, (size_t)m * sizeof(shown[0]));
  a->prev_shown_n = m;

  tokdec_text(a->tok, kept, n, out, size);
}

static void partial(asr_t *a) {
  char text[ASR_TEXT];
  read_text(a, text, sizeof(text));
  if (nl_strcmp(text, a->last) != 0) {
    nl_strncpy_safe(a->last, text, sizeof(a->last));
    a->partials++;
    say(a, 0, text);
  }
}

static void finish(asr_t *a) {
  if (a->loud >= ASR_MIN_LOUD) {
    char text[ASR_TEXT];
    an_flush(a->stream);
    a->last_frames = an_frames(a->stream);
    a->last_blocks = a->blocks;
    read_text(a, text, sizeof(text));
    a->utterances++;
    say(a, 1, text);
  }

  an_stream_reset(a->stream);
  a->speaking = 0;
  a->quiet = 0;
  a->loud = 0;
  a->blocks = 0;
  a->since_part = 0;
  a->run_on = 0;
  a->last[0] = 0;
  a->pre_n = 0;
  a->pre_i = 0;
  a->prev_shown_n = 0;
}

void asr_flush(asr_t *a) {
  if (!a->stream) {
    return;
  }

  if (a->speaking) {
    finish(a);
  }

  a->nblk = 0;
}

/* Gate */

static void block(asr_t *a, const int16_t *b) {
  long sum = 0;
  for (int i = 0; i < ASR_BLOCK; i++) {
    sum += b[i] < 0 ? -(long)b[i] : (long)b[i];
  }

  float level = (float)sum / ASR_BLOCK;
  if (a->floor < 0 || level < a->floor) {
    a->floor = level;
  } else if (!a->speaking) {
    a->floor += (level - a->floor) * 0.002f;
  }

  int loud = level > a->floor * 3.0f + 60.0f;
  if (!a->speaking) {
    nl_memcpy(a->pre[a->pre_i], b, sizeof(a->pre[0]));
    a->pre_i = (a->pre_i + 1) % ASR_PREROLL;
    if (a->pre_n < ASR_PREROLL) {
      a->pre_n++;
    }

    a->run_on = loud ? a->run_on + 1 : 0;
    if (a->run_on < 2) {
      return;
    }

    // Onset: model gets what led up to it as well
    a->speaking = 1;
    a->loud = a->run_on;
    a->quiet = 0;
    a->blocks = 0;
    a->since_part = 0;
    int start = (a->pre_i + ASR_PREROLL - a->pre_n) % ASR_PREROLL;
    for (int i = 0; i < a->pre_n; i++) {
      an_feed_s16(a->stream, a->pre[(start + i) % ASR_PREROLL], ASR_BLOCK);
      a->blocks++;
    }

    return;
  }

  an_feed_s16(a->stream, b, ASR_BLOCK);
  a->blocks++;
  a->since_part++;
  if (loud) {
    a->loud++;
    a->quiet = 0;
  } else {
    a->quiet++;
  }

  if (a->quiet >= ASR_END_MS / 20 || an_full(a->stream) ||
      a->blocks * 20 >= ASR_MAX_MS) {
    finish(a);

    return;
  }

  if (a->since_part >= ASR_PARTIAL_MS / 20) {
    a->since_part = 0;
    partial(a);
  }
}

void asr_feed(asr_t *a, const int16_t *pcm, size_t n) {
  if (!a->stream) {
    return;
  }

  for (size_t i = 0; i < n; i++) {
    a->blk[a->nblk++] = pcm[i];
    if (a->nblk == ASR_BLOCK) {
      a->nblk = 0;
      block(a, a->blk);
    }
  }
}

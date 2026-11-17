/*
 * src/voice_cli.c - enrolling, listing and listening
 */

#include "platform.h"

#include "nolibc.h"

#include "mic.h"
#include "voice_cli.h"

static int voice_path(char *out, size_t size, const voice_opts_t *o,
                      const char *xdg, const char *home) {
  if (o->voice) {
    if (nl_strlen(o->voice) + 1 > size) {
      return -1;
    }

    nl_strncpy_safe(out, o->voice, size);

    return 0;
  }

  return sp_default_path(out, size, xdg, home);
}

int voice_is_command(const voice_opts_t *o) {
  return o->enroll_word || o->list || o->forget;
}

int voice_load(sp_t *sp, const voice_opts_t *o, const char *xdg,
               const char *home) {
  char path[SP_PATH];
  if (voice_path(path, sizeof(path), o, xdg, home) != 0) {
    fprintf(stderr, "voice: no template file (set --voice <file> or HOME)\n");

    return -1;
  }

  if (sp_load(sp, path) != 0) {
    fprintf(stderr, "voice: %s is damaged or not a voice file\n", path);

    return -1;
  }

  return 0;
}

int voice_command(const voice_opts_t *o, const char *xdg, const char *home) {
  static sp_t sp;
  char path[SP_PATH];
  sp_init(&sp);
  if (voice_path(path, sizeof(path), o, xdg, home) != 0) {
    fprintf(stderr, "voice: no template file (set --voice <file> or HOME)\n");

    return 1;
  }

  if (sp_load(&sp, path) != 0) {
    fprintf(stderr, "voice: %s is damaged or not a voice file\n", path);

    return 1;
  }

  int rc = 0;
  if (o->enroll_word) {
    char err[64] = {0};
    int n = sp_enroll(&sp, o->enroll_word, o->enroll_src, err, sizeof(err));
    if (n == -1) {
      fprintf(stderr, "--voice-enroll: cannot read %s\n", o->enroll_src);
      rc = 1;
    } else if (n == -2) {
      fprintf(stderr, "--voice-enroll: %s: %s (use 16 bit PCM)\n",
              o->enroll_src, err);
      rc = 1;
    } else if (n == 0) {
      fprintf(stderr,
              "--voice-enroll: no speech found in %s (too quiet, or "
              "shorter than 0.1 s)\n",
              o->enroll_src);
      rc = 1;
    } else if (sp_save(&sp, path) != 0) {
      fprintf(stderr, "--voice-enroll: cannot write %s\n", path);
      rc = 1;
    } else {
      fprintf(stderr, "\"%s\": %d new, %d of %d kept (%s)\n", o->enroll_word, n,
              sp_count(&sp, o->enroll_word), SP_PER_WORD, path);
    }
  }

  if (rc == 0 && o->forget) {
    int n = sp_remove(&sp, o->forget);
    if (n == 0) {
      fprintf(stderr, "--voice-forget: \"%s\" is not enrolled\n", o->forget);
      rc = 1;
    } else if (sp_save(&sp, path) != 0) {
      fprintf(stderr, "--voice-forget: cannot write %s\n", path);
      rc = 1;
    } else {
      fprintf(stderr, "forgot \"%s\" (%d)\n", o->forget, n);
    }
  }

  if (rc == 0 && o->list) {
    fprintf(stderr, "%s: %d words, %d templates\n", path, sp_words(&sp),
            sp.ntpl);
    for (int i = 0; i < sp.ntpl; i++) {
      int first = 1;
      for (int j = 0; j < i; j++) {
        if (nl_strcmp(sp.tpl[j].word, sp.tpl[i].word) == 0) {
          first = 0;
        }
      }

      if (first) {
        fprintf(stderr, "  %-20s %d\n", sp.tpl[i].word,
                sp_count(&sp, sp.tpl[i].word));
      }
    }
  }

  sp_free(&sp);

  return rc;
}

static void out_line(void *user, int final, const char *text) {
  (void)user;
  char line[ASR_TEXT + 4];
  int n = nl_snprintf(line, sizeof(line), "%s%s\n", final ? "" : "~", text);
  if (n > 0) {
    ssize_t w = write(1, line, (size_t)n);
    (void)w;
  }
}

int voice_prepare(sp_t *sp, asr_t *a, const voice_opts_t *o, const char *xdg,
                  const char *home) {
  if (o->model) {
    char err[128];
    if (asr_open(a, o->model, o->tokenizer, err, sizeof(err)) != 0) {
      fprintf(stderr, "--model: %s: %s\n", o->model, err);

      return -1;
    }

    asr_attach(a, sp);

    return 0;
  }

  if (voice_load(sp, o, xdg, home) != 0) {
    return -1;
  }

  if (sp->ntpl == 0) {
    fprintf(stderr, "--listen: nothing enrolled yet, teach it some words with "
                    "--voice-enroll <word> <audio file>, or use --model for "
                    "full sentences\n");

    return -1;
  }

  return 0;
}

void voice_set_text(sp_t *sp, asr_t *a, const voice_opts_t *o, sp_text_fn fn,
                    void *user) {
  if (o->model) {
    asr_set_text(a, fn, user);
  } else {
    sp_set_text(sp, fn, user);
  }
}

// --mic: read sound card in 20 ms blocks until it fails or we are killed
static int voice_mic_loop(sp_t *sp, asr_t *asr, const voice_opts_t *o) {
  static mic_t mic;
  char err[128];
  if (mic_open(&mic, o->mic, err, sizeof err) != 0) {
    fprintf(stderr, "--mic: %s\n", err);
    asr_close(asr);

    return 1;
  }

  voice_set_text(sp, asr, o, out_line, NULL);
  int16_t block[320];
  int status = 0;
  for (;;) {
    long n = mic_read(&mic, block, sizeof block / sizeof block[0]);
    if (n < 0) {
      fprintf(stderr, "--mic: %s: read failed\n", o->mic);
      status = 1;
      break;
    }

    if (n > 0) {
      sp_feed(sp, block, (size_t)n);
    }
  }

  sp_flush(sp);
  mic_close(&mic);
  sp_free(sp);
  asr_close(asr);

  return status;
}

int voice_listen_main(const voice_opts_t *o, const char *xdg,
                      const char *home) {
  static sp_t sp;
  static asr_t asr;
  static sp_src_t src;
  sp_init(&sp);

  asr_init(&asr);
  if (voice_prepare(&sp, &asr, o, xdg, home) != 0) {
    return 1;
  }

  if (o->mic) {
    return voice_mic_loop(&sp, &asr, o);
  }

  if (sp_src_open(&src, o->listen) != 0) {
    fprintf(stderr, "--listen: cannot open %s\n", o->listen);
    asr_close(&asr);

    return 1;
  }

  voice_set_text(&sp, &asr, o, out_line, NULL);
  while (!src.eof) {
    long n = sp_src_poll(&src, &sp);
    if (!src.st.ok) {
      fprintf(stderr, "--listen: %s: %s\n", o->listen, src.st.err);
      sp_src_close(&src);

      return 1;
    }

    if (n == 0) {
      usleep(20000);
    }
  }

  sp_flush(&sp);
  sp_src_close(&src);
  sp_free(&sp);
  asr_close(&asr);

  return 0;
}

#ifndef VOICE_CLI_H
#define VOICE_CLI_H

/*
 * voice_cli - command line side of speech.h
 *
 *   --voice <f>             template file (default: see sp_default_path)
 *   --voice-enroll <w> <a>  teach word or phrase w from audio file a ("-":
 *                           stdin)
 *   --voice-list            what is enrolled
 *   --voice-forget <w>      remove w
 *   --listen <a>            recognise audio from file or fifo a, as captions
 *   --mic <d>               same, from sound card d: default, hw:C,D or a
 *                           device path (Linux, ALSA; Ctrl-C to stop)
 *   --model <w>             with --listen: full sentences from a neural model
 *                           (weights file w, vocabulary --tokenizer <j>,
 *                           default tokenizer.json beside w) instead of words
 *                           have been taught to it
 *  --asr-debug              with --model: print decoder ids (raw and after
 *                           repeat guard) to stderr, for comparing against a
 *                           reference transcript token by token (weights file
 *                           w, vocabulary --tokenizer <j>, default
 *                           trokenizer.json beside w) instead of words have
 *                           been taught to it
 */

#include "asr.h"
#include "speech.h"

typedef struct voice_opts_s {
  const char *voice;
  const char *listen;
  const char *mic;
  const char *model;
  const char *tokenizer;
  const char *enroll_word;
  const char *enroll_src;
  const char *forget;

  int list;
  int asr_debug; // --asr-debug: dump raw/trimmed decoder ids to stderr
} voice_opts_t;

// Return 1 if enroll, list or forget were asked for: they run and exit
int voice_is_command(const voice_opts_t *o);

// Run that command, returns exit status
int voice_command(const voice_opts_t *o, const char *xdg, const char *home);

// Resolve template file and load it into sp
// Return 0 on success, else a message went to stderr
int voice_load(sp_t *sp, const voice_opts_t *o, const char *xdg,
               const char *home);

// Get recogniser behind --listen ready: model (into a, wired to sp) or enrolled
// words (into sp)
// Return 0 on success, else a message went to stderr
int voice_prepare(sp_t *sp, asr_t *a, const voice_opts_t *o, const char *xdg,
                  const char *home);

// Where recognised text goes, whichever recogniser it is
void voice_set_text(sp_t *sp, asr_t *a, const voice_opts_t *o, sp_text_fn fn,
                    void *user);

// --listen without a call: lines on stdout ("~" starts a partial one), until
// a file ends or forever for a fifo
// Returns exit status
int voice_listen_main(const voice_opts_t *o, const char *xdg, const char *home);

#endif

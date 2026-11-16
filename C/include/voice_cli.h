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
 */

#include "speech.h"

typedef struct voice_opts_s {
  const char *voice;
  const char *listen;
  const char *enroll_word;
  const char *enroll_src;
  const char *forget;
  int list;
} voice_opts_t;

// 1 if enroll, list or forget were asked for: they run and exit
int voice_is_command(const voice_opts_t *o);

// Run that command, returns exit status
int voice_command(const voice_opts_t *o, const char *xdg, const char *home);

// Resolve template file and load it into sp
// Return 0 on success, else a message went to stderr
int voice_load(sp_t *sp, const voice_opts_t *o, const char *xdg,
               const char *home);

// --listen without a call: lines on stdout ("~" starts a partial one), until
// a file ends or forever for a fifo
// Returns exit status
int voice_listen_main(const voice_opts_t *o, const char *xdg, const char *home);

#endif

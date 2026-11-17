#ifndef TOKDEC_H
#define TOKDEC_H

/*
 * tokdec - turn token ids a model produced back into text
 *
 * Reads vocabulary out of a tokenizer.json ("vocab" model, plus "added_tokens")
 * Pieces are joined way SentencePiece style tokenizers do it: : "▁" marker is a
 * space, <0xNN> is one raw byte (so any UTF-8 can be spelled), special tokens
 * like <s> and </s> are dropped. Decoding only: recogniser never has to turn
 * text into ids
 *
 * File is untrusted: sizes are bounded and every string is checked
 */

#include "nl_types.h"

#define TD_MAX_IDS 65536
#define TD_MAX_POOL (4u << 20u)
#define TD_MAX_PIECE 64

typedef struct {
  const char **piece;     // id -> UTF-8 piece, NULL when id has none
  unsigned char *special; // id -> 1 for control tokens that print nothing
  char *pool;
  int n; // ids 0..n-1
} tokdec_t;

// Parse tokenizer.json held in memory
// Return 0 on success, -1 on error (err gets reason, may be NULL)
int tokdec_parse(tokdec_t *t, const char *json, size_t size, char *err,
                 size_t err_size);

void tokdec_free(tokdec_t *t);

// 1 when id prints something: a known id that is not a control token
int tokdec_is_text(const tokdec_t *t, int id);

// Append text of one token to out (NUL terminated, at most size bytes including
// NUL). A byte-fallback token that is start of a multibyte character is kept as
// is: bytes of one character come from consecutive tokens, so result is only
// valid UTF-8 once all of them are in
// Returns bytes appended
size_t tokdec_append(const tokdec_t *t, int id, char *out, size_t out_size,
                     size_t at);

// Text of ids[0..n), with space tokenizer puts before first word removed, and
// any byte sequence that is not valid UTF-8 replaced by '?'
// Returns length
size_t tokdec_text(const tokdec_t *t, const int *ids, int n, char *out,
                   size_t out_size);

#endif

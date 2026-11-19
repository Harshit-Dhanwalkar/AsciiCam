#ifndef asr_h
#define asr_h

/*
 * asr - live recognition of sentences with a neural model, no dependencies
 *
 * ast.h is network; this is everything around it that makes it a live
 * captioner: it cuts incoming audio into utterances by loudness, feeds model as
 * audio arrives (encoder keeps up with speaker), and while someone is talking
 * re-reads what model makes of it so far every asr_partial_ms, telling caller a
 * partial line each time it changes. when speaker pauses utterance is finished
 * and its text goes out as final
 *
 *   audio -> loudness gate -> an_feed ... -> partial (every second)
 *                                         -> pause -> flush -> final
 *
 * models are files: weights (model.safetensors) and vocabulary (tokenizer.json)
 * of model
 */

#include "asrnet.h"
#include "speech.h"
#include "tokdec.h"

#define ASR_BLOCK 320       // 20 ms of audio, unit of loudness gate
#define ASR_PARTIAL_MS 1000 // between partial lines
#define ASR_END_MS 600      // silence that ends an utterance
#define ASR_PREROLL 10      // blocks kept from before onset
#define ASR_MIN_LOUD 8      // loud blocks an utterance needs (160 ms)
#define ASR_MAX_MS 30000    // longest utterance, then it is cut
#define ASR_TEXT 512
#define ASR_REPEAT_MAX 2 // copies of a block over 2 ids long allowed in a row
#define ASR_SHORT_REPEAT_MAX 4 // copies of a 1-2 id block, unless still looping
#define ASR_REPEAT_LEN 8       // longest block looked at for repeats

// Copy n token ids to out, keeping at most short_max copies in a row of a block
// 1-2 ids long (a word, syllable or short phrase - "hi, hi, hi" is often really
// said 3+ times) and at most ASR_REPEAT_MAX copies of a longer block
// (3..ASR_REPEAT_LEN ids - a model stuck looping a phrase, "the story of the
// story of ..."; real speech essentially never repeats one that verbatim)
// Out must hold n ids
// Returns number kept
int asr_trim_repeats(const int *in, int n, int *out, int short_max);

// True if cur is exactly prev (previous call's pre-trim ids, same utterance)
// with only more copies of prev's already-repeating 1-2 id trailing block
// appended and nothing else new - model is stuck looping it, not just repeating
// it normally
// Exposed for tests; read_text() is its only other caller
int asr_still_looping(const int *prev, int pn, const int *cur, int cn);

typedef void (*asr_text_fn)(void *user, int final, const char *text);

typedef struct {
  an_model_t own_model; // when asr_open loaded them
  tokdec_t own_tok;
  int owns;
  const an_model_t *model;
  const tokdec_t *tok;
  an_stream_t *stream;

  asr_text_fn text;
  void *text_user;

  // Gate
  int16_t blk[ASR_BLOCK];
  int nblk;
  int16_t pre[ASR_PREROLL][ASR_BLOCK];
  int pre_n;
  int pre_i;
  float floor; // Noise level, mean absolute sample
  int run_on;  // Consecutive loud blocks
  int speaking;
  int quiet;       // Quiet blocks since last loud one
  int loud;        // Loud blocks this utterance
  long blocks;     // Blocks of this utterance
  long since_part; // Blocks since last partial
  char last[ASR_TEXT];

  long last_frames, last_blocks; // Last utterance: encoder frames, blocks
  int utterances;                // Statistics
  int partials;

  // What last read_text() call saw before trimming, to tell a model stuck
  // looping (every id added since is same block repeating, no new content
  // at all) from a burst of repeated speech
  int prev_shown[256];
  int prev_shown_n;

  int debug; // 1: print raw and repeat-trimmed ids to stderr (--asr-debug)
} asr_t;

void asr_init(asr_t *a);

// Load model weights and vocabulary. if tokenizer is null it is tokenizer.json
// next to weights
// Return 0 on success, -1 with a reason in err
int asr_open(asr_t *a, const char *model_path, const char *tokenizer_path,
             char *err, size_t err_size);

// Use a model already loaded (tests); a keeps pointers to them, not a copy
int asr_use(asr_t *a, const an_model_t *model, const tokdec_t *tok);

void asr_close(asr_t *a);

void asr_set_text(asr_t *a, asr_text_fn fn, void *user);

// 16 khz mono samples
void asr_feed(asr_t *a, const int16_t *pcm, size_t n);

// Audio ended: finish utterance in progress
void asr_flush(asr_t *a);

// Route a speech.h stream into a (sp_on_pcm)
void asr_attach(asr_t *a, sp_t *sp);

#endif

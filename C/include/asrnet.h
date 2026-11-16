#ifndef MOONSHINE_H
#define MOONSHINE_H

/*
 * asrnet - streaming speech recognition network
 *
 * English, 16 kHz: a 5 ms audio frontend, two causal stride-2 convolutions, a
 * sliding-window transformer encoder with no position information, and an
 * autoregressive decoder with rotary embeddings that listens to encoder through
 * cross attention
 * This file runs it from a weights file in safetensors format and a
 * config.json, nothing else needed
 *
 * encoder is computed as audio arrives: an_feed() costs same for every second
 * of speech, whatever came before. Every encoder frame depends on at most 3
 * future frames per layer, so result for audio so far is what whole-file
 * computation gives, apart from last few frames that wait for those (an_flush()
 * ends stream and releases them)
 *
 * One an_model_t (weights) can serve several an_stream_t
 */

#include "nl_types.h"
#include "safetensors.h"

#define AN_FRAME 80     // samples per 5 ms frame at 16 kHz
#define AN_RING 32      // encoder frames of attention history kept per layer
#define AN_MAX_ENC 2048 // encoder frames (20 ms) one stream holds: 41 s
#define AN_MAX_LAYERS 16
#define AN_OWNED 128

typedef struct {
  int hid, ffn, layers, heads;     // encoder
  int dhid, dffn, dlayers, dheads; // decoder
  int vocab;
  int win_l[AN_MAX_LAYERS]; // left per layer
  int win_r[AN_MAX_LAYERS]; // right per layer
  float rope_theta;
  float rope_partial;
  int start_tok;
  int eos_tok;
} an_config_t;

typedef struct {
  const float *q, *k, *v, *o;
} an_attn_t;

typedef struct {
  an_attn_t attn;
  const float *ln1, *ln2; // gamma + 1, ready to multiply
  const float *fc1w, *fc1b, *fc2w, *fc2b;
} an_enc_layer_t;

typedef struct {
  an_attn_t self, cross;
  const float *ln_in, *ln_post, *ln_final;
  const float *fc1w, *fc1b, *fc2w, *fc2b;
} an_dec_layer_t;

typedef struct {
  an_config_t cfg;
  st_t st;
  float log_k;
  const float *lin_w; // frame -> hid, no bias
  float *conv1_w;     // [2hid][5][hid], repacked from [2hid][hid][5]
  const float *conv1_b;
  float *conv2_w; // [hid][5][2hid]
  const float *conv2_b;
  an_enc_layer_t enc[AN_MAX_LAYERS];
  const float *enc_norm; // gamma + 1
  const float *embed;    // [vocab][dhid]
  const float *pos_emb;  // [4096][hid]
  const float *proj;     // [dhid][hid] or NULL
  size_t pos_rows;
  an_dec_layer_t dec[AN_MAX_LAYERS];
  const float *dec_norm;
  const float *proj_out; // [vocab][dhid]
  float *owned[AN_OWNED];
  int nowned;
} an_model_t;

typedef struct an_stream an_stream_t;

// Configuration of smallest model (default)
void an_config_tiny(an_config_t *c);

// Configuration from a config.json: keys that are absent keep smallest model's
// values
// Return 0, or -1 with reason in err
int an_config_parse(an_config_t *c, const char *json, size_t n, char *err,
                    size_t err_size);

// Load weights (safetensors) and check them against cfg (NULL: tiny)
// Return 0 on success, else -1 with a reason in err
int an_load(an_model_t *m, const char *path, const an_config_t *cfg, char *err,
            size_t err_size);

// Same from a parsed file; model borrows st and an_free closes it
int an_load_st(an_model_t *m, const st_t *st, const an_config_t *cfg,
               char *err, size_t err_size);
void an_free(an_model_t *m);

an_stream_t *an_stream_new(const an_model_t *m);
// Same holding at most cap frames (1..an_MAX_ENC), for small tests
an_stream_t *an_stream_new_cap(const an_model_t *m, long cap);
void an_stream_free(an_stream_t *s);
void an_stream_reset(an_stream_t *s);

// Audio as float in [-1, 1], or 16 bit samples
void an_feed(an_stream_t *s, const float *pcm, size_t n);
void an_feed_s16(an_stream_t *s, const int16_t *pcm, size_t n);

// End of audio: frames still waiting for lookahead are finished
// Stream takes no more audio until an_stream_reset
void an_flush(an_stream_t *s);

// Encoder frames available (20 ms each)
long an_frames(const an_stream_t *s);

// Whether stream is full (its capacity in frames, an_MAX_ENC by default): cut
// and reset
int an_full(const an_stream_t *s);

// Greedy decoding of audio so far. Returns number of tokens (at most
// max_tokens, and at most about 6.5 per second of audio, which stops repetition
// loops model is known for)
// Tokens exclude start and end
int an_decode(an_stream_t *s, int *tokens, int max_tokens);

// Encoder output of frame i (hid floats), or NULL
// For tests
const float *an_encoder_frame(const an_stream_t *s, long i);

// Logits of first decoder step after tokens given (vocab floats in out), for
// testing against a reference
int an_decoder_logits(an_stream_t *s, const int *tokens, int n, float *out);

#endif

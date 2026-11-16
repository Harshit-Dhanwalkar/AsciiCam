#ifndef SPEECH_H
#define SPEECH_H

/*
 * speech - live speech recognition for captions, no dependencies
 *
 * This is a speaker-dependent recogniser: it knows words and phrases whcih are
 * been taught to it, in user's voice, and writes them out as caption lines
 * WARN: It is not free dictation but it is pipeline that runs in a few
 * milliseconds per second of audio:
 *
 *   16 kHz mono PCM
 *     -> 25 ms frames every 10 ms, pre-emphasis, Hamming window
 *     -> 512-point FFT -> 26 mel filters -> log -> DCT -> 12 cepstra (liftered)
 *     -> voice activity detection (adaptive noise floor, hangover) cuts stream
 *        into utterances
 *     -> cepstral mean normalisation per utterance
 *     -> dynamic time warping against every enrolled template of every word
 *     -> accept if best word is close enough and clearly ahead of runner-up
 *
 * Audio comes in as bytes: raw 16 kHz mono signed 16-bit little endian, or a
 * WAV file/stream (any sample rate, mono or stereo, 16 bit)
 * `arecord -q -t wav -f S16_LE -r 16000 -c 1 > fifo` is enough to feed it
 *
 * Templates live in one file so that enrolling is
 * `asciicam --voice-enroll yes yes.wav` a few times per word
 */

#include "nl_types.h"

#define SP_RATE 16000
#define SP_FRAME 400 // 25 ms
#define SP_HOP 160   // 10 ms
#define SP_NFFT 512
#define SP_NMEL 26
#define SP_NCEP 12
#define SP_MAXFR 400 // longest utterance, 4 s
#define SP_PREROLL 8 // frames kept from before onset
#define SP_WORD 48
#define SP_MAX_TPL 128
#define SP_PER_WORD 5 // enrolling more replaces oldest of word
#define SP_PATH 256

typedef struct {
  char word[SP_WORD];
  int nfr;
  float *f; // nfr * SP_NCEP, mean normalised
} sp_tpl_t;

// What was heard
typedef void (*sp_text_fn)(void *user, int final, const char *text);

// A finished utterance (features, mean normalised) for sp_on_utterance
typedef void (*sp_utt_fn)(void *user, const float *f, int nfr);

// Samples as they come, and (NULL, 0) when the audio ends: see sp_on_pcm
typedef void (*sp_pcm_fn)(void *user, const int16_t *pcm, size_t n);

typedef struct {
  // Template store
  sp_tpl_t tpl[SP_MAX_TPL];
  int ntpl;

  // Tuning: best distance must be under `thresh`, and nearest other word
  // must be at least `margin` (ratio) further away
  float thresh;
  float margin;

  // Frame assembly
  float pcm[SP_FRAME]; // last frame of samples
  int npcm;            // samples collected towards next frame
  float prev;          // last sample of previous frame (pre-emphasis)

  // VAD
  float blockmin[6]; // noise floor is lowest of these 50-frame blocks
  float curmin;
  int curn;
  int run_on;   // consecutive loud frames
  int quiet;    // consecutive quiet frames while in speech
  int loud;     // frames above threshold in this utterance
  int speaking; // inside an utterance
  float pre[SP_PREROLL][SP_NCEP];
  int pre_n, pre_i;

  // Utterance being collected
  float feat[SP_MAXFR][SP_NCEP];
  int nfr;
  int nlead; // frames held back as possible trailing silence
  int part_at;
  char part_last[SP_WORD];

  sp_text_fn text;
  void *text_user;
  sp_pcm_fn pcm_fn; // when set, samples go here and nothing is recognised
  void *pcm_user;
  sp_utt_fn utt; // when set, utterances go here instead of being recognised
  void *utt_user;

  int utterances, accepted; // statistics
  float last_dist, last_margin;
  char last_word[SP_WORD];
} sp_t;

void sp_init(sp_t *s);
void sp_free(sp_t *s);

// Where text goes; NULL fn drops it
void sp_set_text(sp_t *s, sp_text_fn fn, void *user);

// Hand utterance features to fn and skip recognition (used to enrol)
void sp_on_utterance(sp_t *s, sp_utt_fn fn, void *user);

// Take over 16 kHz samples (for a recogniser of sentences): sp_feed hands them
// to fn instead of its own recognition, and sp_flush calls fn(user, NULL, 0)
// Return NULL fn goes back to templates
void sp_on_pcm(sp_t *s, sp_pcm_fn fn, void *user);

// 16 kHz mono samples
void sp_feed(sp_t *s, const int16_t *pcm, size_t n);

// Close utterance in progress, if any (end of a file)
void sp_flush(sp_t *s);

// Forget signal state, keep templates
void sp_reset(sp_t *s);

/* Templates */

// Add a template; oldest of this word goes when it already has SP_PER_WORD
// Returns 0, or -1 for an empty word, nfr out of range or no room
int sp_add(sp_t *s, const char *word, const float *f, int nfr);

int sp_words(const sp_t *s); // distinct words
int sp_count(const sp_t *s, const char *word);

// Remove all templates of word; number removed
int sp_remove(sp_t *s, const char *word);

// Load/save store; load adds to what is there
// Return 0 on success, -1 on error, load of a missing file is 0 with nothing
// added
int sp_load(sp_t *s, const char *path);
int sp_save(const sp_t *s, const char *path); // creates directories

// $XDG_CONFIG_HOME/asciicam/voice.tpl, else $HOME/.config/asciicam/voice.tpl
// Return 0 on success, -1 when it does not fit or there is no home
int sp_default_path(char *out, size_t size, const char *xdg, const char *home);

// Teach word from an audio file (raw PCM or WAV; "-" is stdin): every utterance
// in it becomes a template, so one recording of "yes, yes, yes" is three
// Phrases are one utterance: speak them without a pause over 0.3 s
// Returns how many were added, -1 when file can not be read, -2 when its
// format is not usable (err gets reason, may be NULL)
int sp_enroll(sp_t *s, const char *word, const char *path, char *err,
              size_t err_size);

// Distance between two utterances (features), normalised by length
// If open_end, template may go on after utterance ends (for partial results)
// Large when nothing matches
// Exposed for tests
float sp_dtw(const float *q, int n, const float *t, int m, int open_end);

// Best word for features: returns its index in tpl[] or -1 if rejected
// dist/margin out (may be NULL)
int sp_match(const sp_t *s, const float *f, int nfr, float *dist,
             float *margin);

/* Bytes in, as they come off a pipe: raw PCM or a WAV (detected by "RIFF")
 * Converts to 16 kHz mono and feeds a recogniser */
typedef struct {
  int state; // header parsing
  unsigned char hdr[48];
  int nh;
  unsigned long skip; // bytes of an unwanted chunk left to skip
  int rate, chans, bits;
  int odd;  // a half sample left over
  int lo;   // its first byte
  int fi;   // channel being read
  long mix; // sum of channels of the current frame
  long acc; // box filter for resampling
  int cnt;
  long phase;
  int last; // last output sample, repeated when upsampling
  int ok;   // 0 when the format is not usable
  char err[64];
} sp_stream_t;

void sp_stream_init(sp_stream_t *st);
void sp_stream_feed(sp_stream_t *st, sp_t *s, const unsigned char *data,
                    size_t n);

/* A file or fifo as audio source, polled */
typedef struct {
  int fd;
  int seekable; // a regular file or stdin: it ends, a fifo does not
  int eof;      // seekable and everything was read
  sp_stream_t st;
} sp_src_t;

// Non-blocking, like capsrc_open (a fifo is opened read-write so it does not
// end with its writer)
// Return 0 on success
int sp_src_open(sp_src_t *src, const char *path);

// Read what is there and feed it; bytes read
long sp_src_poll(sp_src_t *src, sp_t *s);

void sp_src_close(sp_src_t *src);

#endif

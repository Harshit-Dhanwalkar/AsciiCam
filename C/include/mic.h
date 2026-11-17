#ifndef mic_h
#define mic_h

/*
 * mic - live audio from a sound card, no dependencies
 *
 * On Linux this talks to ALSA straight through its kernel interface
 * (/dev/snd/pcmC<card>D<dev>c), so no alsa-lib is needed
 * It asks card for 16 kHz mono signed 16-bit samples, rate recogniser takes;
 * cards that refuse that get 48 kHz, which is box-averaged down by three, and
 * stereo, which is averaged to mono
 * Other platforms get an error from mic_open
 *
 * Kernel structs below are copied from <sound/asound.h> and checked by
 * tests/test_mic.c against ioctl numbers that header defines.
 */

#include "nolibc.h"

#include <stdint.h>

// Kernel ABI (sound/asound.h). Sizes are for LP64 and ILP32 Linux alike.
#define MIC_MASK_WORDS 8 // SNDRV_MASK_MAX 256 bits

typedef struct mic_mask_s {
  uint32_t bits[MIC_MASK_WORDS];
} mic_mask_t;

typedef struct mic_interval_s {
  uint32_t min, max;
  unsigned int openmin : 1, openmax : 1, integer : 1, empty : 1;
} mic_interval_t;

typedef struct mic_hw_params_s {
  uint32_t flags;
  mic_mask_t masks[3];      // Access, format, subformat
  mic_mask_t mres[5];       // Reserved
  mic_interval_t ivals[12]; // sample_bits .. tick_time
  mic_interval_t ires[9];   // Reserved
  uint32_t rmask;
  uint32_t cmask;
  uint32_t info;
  uint32_t msbits;
  uint32_t rate_num;
  uint32_t rate_den;
  unsigned long fifo_size; // snd_pcm_uframes_t
  unsigned char reserved[64];
} mic_hw_params_t;

#define MIC_HW_ACCESS 0
#define MIC_HW_FORMAT 1
#define MIC_HW_SUBFORMAT 2
#define MIC_HW_CHANNELS 10
#define MIC_HW_RATE 11
#define MIC_IVAL(param) ((param) - 8) // Index into ivals[] (first is 8)

#define MIC_ACCESS_RW_INTERLEAVED 3
#define MIC_FORMAT_S16_LE 2
#define MIC_STREAM_CAPTURE 1

#define MIC_IOC(dir, type, nr, size)                                           \
  (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) |              \
   ((unsigned long)(type) << 8) | (unsigned long)(nr))
#define MIC_IOC_HW_PARAMS MIC_IOC(3, 'A', 0x11, sizeof(mic_hw_params_t))
#define MIC_IOC_PREPARE MIC_IOC(0, 'A', 0x40, 0)
#define MIC_IOC_START MIC_IOC(0, 'A', 0x42, 0)
#define MIC_IOC_DROP MIC_IOC(0, 'A', 0x43, 0)

#define MIC_RATE 16000
#define MIC_RATE_WIDE 48000 // Fallback, decimated by MIC_WIDE_DECIM
#define MIC_WIDE_DECIM 3
#define MIC_RAW_FRAMES 4096 // Largest single read, in card frames

typedef struct mic_s {
  int fd;
  int channels; // As card runs it: 1 or 2
  int decim;    // 1, or MIC_WIDE_DECIM
  int16_t raw[MIC_RAW_FRAMES * 2];
} mic_t;

// Map "default", "hw:<card>,<dev>" or an absolute path to a device node
// Returns 0 and fills out, or -1 if name is not one of those forms
int mic_device_path(const char *dev, char *out, size_t size);

// Average interleaved S16 frames to mono and keep every decim-th group in holds
// nframes * channels samples; out needs nframes / decim room
// Returns number of mono samples written.
size_t mic_convert(const int16_t *in, size_t nframes, int channels, int decim,
                   int16_t *out);

// Open a capture device and start it
// On failure returns -1 and writes a one-line reason to err
int mic_open(mic_t *m, const char *dev, char *err, size_t errn);

// Read next block of 16 kHz mono samples into out (room for n)
// Blocks until card has data
// Returns count (may be less than n and 0 after an overrun was recovered), or
// -1 on error
long mic_read(mic_t *m, int16_t *out, size_t n);

void mic_close(mic_t *m);

#endif

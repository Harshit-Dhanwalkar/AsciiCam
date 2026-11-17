/*
 * mic.c - ALSA capture through kernel interface
 */

#include "nolibc.h"

#include "mic.h"

#include "nl_string.h"

#define MIC_EPIPE 32 // ALSA reports overrun/underrun as EPIPE

static void mic_fail(char *err, size_t errn, const char *what,
                     const char *path) {
  if (!err || errn == 0) {
    return;
  }

  size_t k = 0;
  for (const char *s = what; *s && k + 1 < errn; s++) {
    err[k++] = *s;
  }
  for (const char *s = path; path && *s && k + 1 < errn; s++) {
    err[k++] = *s;
  }

  err[k] = 0;
}

static int mic_parse_uint(const char **p, unsigned *v) {
  const char *s = *p;
  unsigned x = 0;
  int digits = 0;
  while (*s >= '0' && *s <= '9') {
    x = x * 10 + (unsigned)(*s - '0');
    s++;
    digits++;
  }

  if (digits == 0) {
    return -1;
  }

  *v = x;
  *p = s;

  return 0;
}

static int mic_put_uint(char *out, size_t size, size_t k, unsigned v) {
  char tmp[12];
  int d = 0;
  do {
    tmp[d++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && d < (int)sizeof tmp);

  while (d > 0) {
    if (k + 1 >= size) {
      return -1;
    }
    out[k++] = tmp[--d];
  }

  out[k] = 0;

  return (int)k;
}

int mic_device_path(const char *dev, char *out, size_t size) {
  if (!dev || !out || size == 0) {
    return -1;
  }

  if (strcmp(dev, "default") == 0) {
    dev = "hw:0,0";
  }

  if (dev[0] == '/') {
    if (strlen(dev) + 1 > size) {
      return -1;
    }
    memcpy(out, dev, strlen(dev) + 1);

    return 0;
  }

  if (dev[0] != 'h' || dev[1] != 'w' || dev[2] != ':') {
    return -1;
  }

  const char *p = dev + 3;
  unsigned card = 0;
  unsigned sub = 0;
  if (mic_parse_uint(&p, &card) != 0 || *p != ',') {
    return -1;
  }
  p++;
  if (mic_parse_uint(&p, &sub) != 0 || *p != 0) {
    return -1;
  }

  const char *pre = "/dev/snd/pcmC";
  size_t k = strlen(pre);
  if (k + 1 >= size) {
    return -1;
  }

  memcpy(out, pre, k);
  out[k] = 0;
  if (mic_put_uint(out, size, k, card) < 0) {
    return -1;
  }

  k = strlen(out);
  out[k++] = 'D';
  if (mic_put_uint(out, size, k, sub) < 0) {
    return -1;
  }

  k = strlen(out);
  if (k + 2 > size) {
    return -1;
  }

  out[k++] = 'c';
  out[k] = 0;

  return 0;
}

size_t mic_convert(const int16_t *in, size_t nframes, int channels, int decim,
                   int16_t *out) {
  if (!in || !out || decim < 1 || (channels != 1 && channels != 2)) {
    return 0;
  }

  size_t n = nframes / (size_t)decim;
  for (size_t j = 0; j < n; j++) {
    int32_t acc = 0;
    for (int k = 0; k < decim; k++) {
      size_t f = j * (size_t)decim + (size_t)k;
      int32_t v;
      if (channels == 2) {
        v = ((int32_t)in[2 * f] + (int32_t)in[2 * f + 1]) / 2;
      } else {
        v = in[f];
      }
      acc += v;
    }

    out[j] = (int16_t)(acc / decim);
  }

  return n;
}

#if defined(PLATFORM_LINUX)

#include "nl_io.h"

// Ask card for rate x channels S16_LE interleaved
// Return 0 on success
static int mic_set_hw(int fd, unsigned rate, int channels) {
  mic_hw_params_t hw;
  memset(&hw, 0, sizeof hw);
  hw.rmask = 0xffffffffu;
  for (int i = 0; i < 3; i++) {
    for (int w = 0; w < MIC_MASK_WORDS; w++) {
      hw.masks[i].bits[w] = 0xffffffffu;
    }
  }
  for (int i = 0; i < 12; i++) {
    hw.ivals[i].min = 0;
    hw.ivals[i].max = 0xffffffffu;
  }

  memset(hw.masks[MIC_HW_ACCESS].bits, 0, sizeof hw.masks[0].bits);
  hw.masks[MIC_HW_ACCESS].bits[0] = 1u << MIC_ACCESS_RW_INTERLEAVED;
  memset(hw.masks[MIC_HW_FORMAT].bits, 0, sizeof hw.masks[0].bits);
  hw.masks[MIC_HW_FORMAT].bits[0] = 1u << MIC_FORMAT_S16_LE;

  mic_interval_t *ch = &hw.ivals[MIC_IVAL(MIC_HW_CHANNELS)];
  ch->min = ch->max = (uint32_t)channels;
  ch->integer = 1;
  mic_interval_t *rt = &hw.ivals[MIC_IVAL(MIC_HW_RATE)];
  rt->min = rt->max = rate;
  rt->integer = 1;

  return ioctl(fd, MIC_IOC_HW_PARAMS, &hw) < 0 ? -1 : 0;
}

int mic_open(mic_t *m, const char *dev, char *err, size_t errn) {
  memset(m, 0, sizeof *m);
  m->fd = -1;

  char path[128];
  if (mic_device_path(dev, path, sizeof path) != 0) {
    mic_fail(err, errn, "not a sound device (use default, hw:C,D or a path): ",
             dev ? dev : "");

    return -1;
  }

  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    mic_fail(err, errn, "cannot open ", path);

    return -1;
  }

  static const struct {
    unsigned rate;
    int channels;
    int decim;
  } tries[] = {
      {MIC_RATE, 1, 1},
      {MIC_RATE, 2, 1},
      {MIC_RATE_WIDE, 1, MIC_WIDE_DECIM},
      {MIC_RATE_WIDE, 2, MIC_WIDE_DECIM},
  };
  int ok = 0;
  for (size_t i = 0; i < sizeof tries / sizeof tries[0] && !ok; i++) {
    if (mic_set_hw(fd, tries[i].rate, tries[i].channels) != 0) {
      continue;
    }
    m->channels = tries[i].channels;
    m->decim = tries[i].decim;
    ok = 1;
  }
  if (!ok) {
    close(fd);
    mic_fail(err, errn, "no 16 kHz or 48 kHz mono/stereo S16 capture on ",
             path);

    return -1;
  }

  if (ioctl(fd, MIC_IOC_PREPARE, 0) < 0 || ioctl(fd, MIC_IOC_START, 0) < 0) {
    close(fd);
    mic_fail(err, errn, "cannot start capture on ", path);

    return -1;
  }

  m->fd = fd;

  return 0;
}

long mic_read(mic_t *m, int16_t *out, size_t n) {
  if (!m || m->fd < 0 || !out || n == 0) {
    return -1;
  }

  size_t frames = n * (size_t)m->decim;
  if (frames > MIC_RAW_FRAMES) {
    frames = MIC_RAW_FRAMES;
  }
  size_t bytes = frames * (size_t)m->channels * sizeof(int16_t);

  ssize_t r = read(m->fd, m->raw, bytes);
  if (r < 0) {
    if (errno == MIC_EPIPE) {
      // overrun: card stopped, so prepare and start it again
      if (ioctl(m->fd, MIC_IOC_PREPARE, 0) < 0 ||
          ioctl(m->fd, MIC_IOC_START, 0) < 0) {
        return -1;
      }

      return 0;
    }

    return -1;
  }

  size_t got = (size_t)r / ((size_t)m->channels * sizeof(int16_t));

  return (long)mic_convert(m->raw, got, m->channels, m->decim, out);
}

void mic_close(mic_t *m) {
  if (m && m->fd >= 0) {
    ioctl(m->fd, MIC_IOC_DROP, 0);
    close(m->fd);
    m->fd = -1;
  }
}

#else

int mic_open(mic_t *m, const char *dev, char *err, size_t errn) {
  (void)dev;
  if (m) {
    m->fd = -1;
  }
  mic_fail(err, errn, "microphone capture is Linux (ALSA) only for now", "");

  return -1;
}

long mic_read(mic_t *m, int16_t *out, size_t n) {
  (void)m;
  (void)out;
  (void)n;

  return -1;
}

void mic_close(mic_t *m) {
  if (m) {
    m->fd = -1;
  }
}

#endif

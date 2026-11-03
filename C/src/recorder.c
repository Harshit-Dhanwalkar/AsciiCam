/*
 * src/recorder.c - screenshots and recordings, see include/recorder.h
 */

#include "platform.h"

#if !defined(__LINUX_NOLIBC__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(PLATFORM_WINDOWS)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif
#endif

#include "nolibc.h"

#include "nl_checked.h"
#include "nl_crc.h"
#include "recorder.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef O_EXCL
#define O_EXCL 0200
#endif

#define PNG_MAX_DIM 16384
#define PNG_MAX_RAW ((size_t)1 << 30) // keeps IDAT length far below 4 GiB

// Helpers

static int write_full(int fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) {
      return -1;
    }

    p += w;
    n -= (size_t)w;
  }

  return 0;
}

// nl_snprintf has no zero padding ("%02d" pads with spaces) and returns the
// bytes it stored, not the length it wanted, so numbers are written here and
// "n + 1 >= size" is treated as truncated

// Unsigned number, zero padded to at least `width` digits. Returns length
static size_t put_num(char *o, unsigned long long v, int width) {
  char t[24];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);

  size_t len = 0;
  for (int i = n; i < width; i++) {
    o[len++] = '0';
  }

  while (n > 0) {
    o[len++] = t[--n];
  }

  return len;
}

// Paths and Time

int rec_default_dir(char *out, size_t outsz, const char *home,
                    const char *xdg_data) {
  int n;
  if (xdg_data && xdg_data[0] == '/') {
    n = nl_snprintf(out, outsz, "%s/asciicam/captures", xdg_data);
  } else if (home && home[0] == '/') {
    n = nl_snprintf(out, outsz, "%s/.local/share/asciicam/captures", home);
  } else {
    n = nl_snprintf(out, outsz, "captures");
  }

  return (n < 0 || (size_t)n + 1 >= outsz) ? -1 : 0;
}

int rec_make_dirs(const char *path) {
  char tmp[REC_PATH_LEN];
  size_t len = nl_strlen(path);
  if (len == 0 || len >= sizeof(tmp)) {
    return -1;
  }

  nl_memcpy(tmp, path, len + 1);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755); // exists already is fine, the last call decides
      *p = '/';
    }
  }

  if (mkdir(tmp, 0755) == 0) {
    return 0;
  }

  // already there? then opening it as a directory works; probe with a file
  char probe[REC_PATH_LEN + 16];
  nl_snprintf(probe, sizeof(probe), "%s/.probe", tmp);
  int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
  if (fd < 0) {
    return -1;
  }

  close(fd);
  unlink(probe);

  return 0;
}

void rec_timestamp(char out[17], int64_t unix_secs) {
  if (unix_secs < 0) {
    unix_secs = 0;
  }

  int64_t days = unix_secs / 86400;
  int64_t rem = unix_secs % 86400;

  // civil-from-days (Howard Hinnant), proleptic Gregorian
  int64_t z = days + 719468;
  int64_t era = z / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) {
    y++;
  }

  if (y > 9999) {
    y = 9999;
  }

  size_t o = put_num(out, (unsigned long long)y, 4);
  o += put_num(out + o, (unsigned long long)m, 2);
  o += put_num(out + o, (unsigned long long)d, 2);
  out[o++] = 'T';
  o += put_num(out + o, (unsigned long long)(rem / 3600), 2);
  o += put_num(out + o, (unsigned long long)(rem % 3600 / 60), 2);
  o += put_num(out + o, (unsigned long long)(rem % 60), 2);
  out[o++] = 'Z';
  out[o] = '\0';
}

int64_t rec_unix_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }

  return (int64_t)ts.tv_sec;
}

uint64_t rec_mono_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }

  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void rec_clock(char out[16], double seconds) {
  unsigned long long s = seconds < 0 ? 0 : (unsigned long long)seconds;
  if (s > 99999ull * 60) {
    s = 99999ull * 60;
  }

  size_t o = put_num(out, s / 60, 1);
  out[o++] = ':';
  o += put_num(out + o, s % 60, 2);
  out[o] = '\0';
}

// Open dir/asciicam-<ts><ext>, adding -1, -2... if two captures share a second.
// Never overwrites: O_EXCL
static int open_unique(const char *dir, int64_t secs, const char *ext,
                       char *path, size_t pathsz, int flags) {
  char ts[17];
  rec_timestamp(ts, secs);
  for (int i = 0; i < 100; i++) {
    int n;
    if (i == 0) {
      n = nl_snprintf(path, pathsz, "%s/asciicam-%s%s", dir, ts, ext);
    } else {
      n = nl_snprintf(path, pathsz, "%s/asciicam-%s-%d%s", dir, ts, i, ext);
    }

    if (n < 0 || (size_t)n + 1 >= pathsz) {
      return -1;
    }

    int fd = open(path, flags | O_CREAT | O_EXCL | O_BINARY, 0644);
    if (fd >= 0) {
      return fd;
    }
  }

  return -1;
}

// Recording (.cast)

void rec_init(recorder_t *r) {
  nl_memset(r, 0, sizeof(*r));
  r->fd = -1;
}

int rec_start(recorder_t *r, const char *dir, int term_w, int term_h,
              int64_t unix_secs, uint64_t now_ns) {
  if (r->active) {
    return -1;
  }

  if (term_w < 1 || term_h < 1 || rec_make_dirs(dir) < 0) {
    return -1;
  }

  char path[REC_PATH_LEN];
  int fd = open_unique(dir, unix_secs, ".cast", path, sizeof(path), O_WRONLY);
  if (fd < 0) {
    return -1;
  }

  char head[192];
  int n = nl_snprintf(head, sizeof(head),
                      "{\"version\": 2, \"width\": %d, \"height\": %d, "
                      "\"timestamp\": %lld, \"env\": {\"TERM\": "
                      "\"xterm-256color\"}}\n",
                      term_w, term_h, (long long)unix_secs);
  if (n <= 0 || write_full(fd, head, (size_t)n) < 0) {
    close(fd);
    unlink(path);

    return -1;
  }

  rec_init(r);
  r->fd = fd;
  r->active = 1;
  r->t0_ns = now_ns;
  r->bytes = (uint64_t)n;
  nl_memcpy(r->path, path, nl_strlen(path) + 1);

  return 0;
}

// Escape one byte for a JSON string. UTF-8 bytes >= 0x80 pass through
static size_t json_byte(unsigned char c, char *o) {
  static const char hx[] = "0123456789abcdef";
  if (c == '"' || c == '\\') {
    o[0] = '\\';
    o[1] = (char)c;

    return 2;
  }

  if (c < 0x20) {
    o[0] = '\\';
    o[1] = 'u';
    o[2] = '0';
    o[3] = '0';
    o[4] = hx[c >> 4];
    o[5] = hx[c & 15];

    return 6;
  }

  o[0] = (char)c;

  return 1;
}

int rec_frame(recorder_t *r, uint64_t now_ns, const char *data, size_t len) {
  if (!r->active || r->fd < 0) {
    return -1;
  }

  uint32_t crc = nl_crc32_update(0, data, len);
  if (r->frames > 0 && crc == r->last_crc && len == r->last_len) {
    r->skipped++;

    return 1;
  }

  uint64_t us = now_ns > r->t0_ns ? (now_ns - r->t0_ns) / 1000 : 0;
  char buf[4096];
  size_t fill = 0;
  buf[fill++] = '[';
  fill += put_num(buf + fill, us / 1000000, 1);
  buf[fill++] = '.';
  fill += put_num(buf + fill, us % 1000000, 6);
  nl_memcpy(buf + fill, ", \"o\", \"", 8);
  fill += 8;

  for (size_t i = 0; i < len; i++) {
    if (fill + 8 > sizeof(buf)) {
      if (write_full(r->fd, buf, fill) < 0) {
        goto fail;
      }

      r->bytes += fill;
      fill = 0;
    }

    fill += json_byte((unsigned char)data[i], buf + fill);
  }

  if (fill + 4 > sizeof(buf)) {
    if (write_full(r->fd, buf, fill) < 0) {
      goto fail;
    }

    r->bytes += fill;
    fill = 0;
  }

  buf[fill++] = '"';
  buf[fill++] = ']';
  buf[fill++] = '\n';
  if (write_full(r->fd, buf, fill) < 0) {
    goto fail;
  }

  r->bytes += fill;
  r->frames++;
  r->last_crc = crc;
  r->last_len = len;

  return 0;

fail:
  close(r->fd);
  r->fd = -1;
  r->active = 0;

  return -1;
}

int rec_stop(recorder_t *r, uint64_t now_ns, double *seconds) {
  if (!r->active) {
    return -1;
  }

  if (seconds) {
    *seconds = now_ns > r->t0_ns ? (double)(now_ns - r->t0_ns) / 1e9 : 0.0;
  }

  if (r->fd >= 0) {
    close(r->fd);
  }

  r->fd = -1;
  r->active = 0;

  return (int)r->frames;
}

// PNG

static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// One whole chunk from memory
static int png_chunk(int fd, const char type[4], const uint8_t *data,
                     uint32_t len) {
  uint8_t head[8];
  be32(head, len);
  nl_memcpy(head + 4, type, 4);
  uint32_t crc = nl_crc32_update(0, head + 4, 4);
  crc = nl_crc32_update(crc, data, len);
  uint8_t tail[4];
  be32(tail, crc);

  if (write_full(fd, head, 8) < 0 || (len && write_full(fd, data, len) < 0) ||
      write_full(fd, tail, 4) < 0) {
    return -1;
  }

  return 0;
}

// Byte `pos` of the raw stream: per row a 0 filter byte, then the row
static void raw_copy(const uint8_t *px, size_t rowlen, size_t pos, size_t n,
                     uint8_t *out) {
  size_t stride = rowlen + 1;
  size_t row = pos / stride;
  size_t col = pos % stride;
  while (n > 0) {
    if (col == 0) {
      *out++ = 0;
      n--;
      col = 1;

      continue;
    }

    size_t take = stride - col;
    if (take > n) {
      take = n;
    }

    nl_memcpy(out, px + row * rowlen + (col - 1), take);
    out += take;
    n -= take;
    col += take;
    if (col == stride) {
      col = 0;
      row++;
    }
  }
}

int png_write(const char *path, int w, int h, int channels,
              const uint8_t *pixels) {
  if (!pixels || w < 1 || h < 1 || w > PNG_MAX_DIM || h > PNG_MAX_DIM ||
      (channels != 1 && channels != 3)) {
    return -1;
  }

  size_t rowlen, stride, raw;
  if (nl_size_mul((size_t)w, (size_t)channels, &rowlen) < 0 ||
      nl_size_add(rowlen, 1, &stride) < 0 ||
      nl_size_mul(stride, (size_t)h, &raw) < 0 || raw > PNG_MAX_RAW) {
    return -1;
  }

  char tmp[REC_PATH_LEN + 8];
  int tn = nl_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if (tn < 0 || (size_t)tn >= sizeof(tmp)) {
    return -1;
  }

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
  if (fd < 0) {
    return -1;
  }

  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t ihdr[13];
  be32(ihdr, (uint32_t)w);
  be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8;                        // bit depth
  ihdr[9] = channels == 3 ? 2 : 0;    // RGB / grey
  ihdr[10] = ihdr[11] = ihdr[12] = 0; // deflate, no filter method, no interlace

  // IDAT = zlib header + stored blocks (max 65535 bytes each) + Adler-32
  size_t nblocks = (raw + 65534) / 65535;
  size_t idat_len = 2 + raw + 5 * nblocks + 4;

  int ok = write_full(fd, sig, 8) == 0 && png_chunk(fd, "IHDR", ihdr, 13) == 0;
  if (ok) {
    uint8_t head[8 + 2];
    be32(head, (uint32_t)idat_len);
    nl_memcpy(head + 4, "IDAT", 4);
    head[8] = 0x78; // deflate, 32K window
    head[9] = 0x01; // no compression, check bits make 0x7801 % 31 == 0
    uint32_t crc = nl_crc32_update(0, head + 4, 6);
    ok = write_full(fd, head, 10) == 0;

    static uint8_t blk[5 + 65535];
    uint32_t adler = 1;
    size_t pos = 0;
    while (ok && pos < raw) {
      size_t n = raw - pos < 65535 ? raw - pos : 65535;
      blk[0] = (pos + n == raw) ? 1 : 0; // BFINAL, BTYPE 00
      blk[1] = (uint8_t)(n & 0xFF);
      blk[2] = (uint8_t)(n >> 8);
      blk[3] = (uint8_t)(~n & 0xFF);
      blk[4] = (uint8_t)((~n >> 8) & 0xFF);
      raw_copy(pixels, rowlen, pos, n, blk + 5);
      adler = nl_adler32_update(adler, blk + 5, n);
      crc = nl_crc32_update(crc, blk, 5 + n);
      ok = write_full(fd, blk, 5 + n) == 0;
      pos += n;
    }

    if (ok) {
      uint8_t tail[8];
      be32(tail, adler);
      crc = nl_crc32_update(crc, tail, 4);
      be32(tail + 4, crc);
      ok = write_full(fd, tail, 8) == 0;
    }
  }

  ok = ok && png_chunk(fd, "IEND", (const uint8_t *)"", 0) == 0;
  close(fd);
  if (!ok) {
    unlink(tmp);

    return -1;
  }

#if defined(PLATFORM_WINDOWS)
  unlink(path); // rename() won't replace an existing file there
#endif
  if (rename(tmp, path) < 0) {
    unlink(tmp);

    return -1;
  }

  return 0;
}

// Screenshot

int shot_save(const char *dir, int64_t unix_secs, const uint8_t *pixels, int w,
              int h, int channels, const char *ansi, size_t ansi_len,
              int term_h, char *png_path, size_t png_sz, char *ansi_path,
              size_t ansi_sz) {
  if (png_sz) {
    png_path[0] = '\0';
  }

  if (ansi_sz) {
    ansi_path[0] = '\0';
  }

  if (rec_make_dirs(dir) < 0) {
    return -1;
  }

  int rc = 0;

  // .ansi first: it is what claims the name, the .png then follows it
  char apath[REC_PATH_LEN];
  int afd =
      open_unique(dir, unix_secs, ".ansi", apath, sizeof(apath), O_WRONLY);
  if (afd < 0) {
    return -1;
  }

  char tail[32];
  int tn = nl_snprintf(tail, sizeof(tail), "\033[0m\033[%d;1H\n",
                       term_h > 0 ? term_h : 1);
  int ok = write_full(afd, "\033[2J\033[H", 7) == 0 &&
           (ansi_len == 0 || write_full(afd, ansi, ansi_len) == 0) &&
           write_full(afd, tail, (size_t)tn) == 0;
  close(afd);
  if (!ok) {
    unlink(apath);

    return -1;
  }

  if (ansi_sz) {
    nl_snprintf(ansi_path, ansi_sz, "%s", apath);
  }

  if (pixels) {
    // same stem as the .ansi so the pair belongs together
    char ppath[REC_PATH_LEN];
    size_t al = nl_strlen(apath);
    nl_memcpy(ppath, apath, al + 1);
    nl_memcpy(ppath + al - 5, ".png", 5); // ".ansi" -> ".png"
    if (png_write(ppath, w, h, channels, pixels) < 0) {
      rc = -1;
    } else if (png_sz) {
      nl_snprintf(png_path, png_sz, "%s", ppath);
    }
  }

  return rc;
}

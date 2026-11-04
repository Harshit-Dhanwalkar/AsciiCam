/*
 * tests/test_recorder.c - src/recorder.c, lib/nl_crc.h: CRC/Adler check
 * values, UTC timestamps (leap days, year ends), save directory choice, PNG
 * encoder checked by parsing file back (signature, chunk CRCs, stored blocks,
 * Adler-32, pixels), .cast escaping / duplicate skipping / timing, and
 * never-overwrite and failure paths
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_recorder.c src/recorder.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/tr
 */
#include "platform.h"

#if !defined(__LINUX_NOLIBC__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../lib/nl_crc.h"
#include "../lib/nolibc.h"
#include "recorder.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#define DIR "build/tests/rec_tmp"

static int same(const char *a, const char *b) { return nl_strcmp(a, b) == 0; }

static int has(const char *hay, const char *needle) {
  size_t nl = nl_strlen(needle);
  for (size_t i = 0; hay[i]; i++) {
    size_t k = 0;
    while (k < nl && hay[i + k] == needle[k]) {
      k++;
    }

    if (k == nl) {
      return 1;
    }
  }

  return 0;
}

static uint8_t fbuf[1 << 20];

// Whole file into fbuf (NUL terminated); size or -1
static long slurp(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  long total = 0;
  for (;;) {
    ssize_t n = read(fd, fbuf + total, sizeof(fbuf) - 1 - (size_t)total);
    if (n <= 0) {
      break;
    }

    total += n;
  }

  close(fd);
  fbuf[total] = 0;

  return total;
}

static int exists(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }

  close(fd);

  return 1;
}

static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

// Parse fbuf as PNG written by png_write and compare pixels. Returns 1 if all
// is right, else prints why
static int png_verify(long size, int w, int h, int ch, const uint8_t *px) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (size < 8 + 25 + 12) {
    fprintf(stderr, "  png too short\n");

    return 0;
  }

  for (int i = 0; i < 8; i++) {
    if (fbuf[i] != sig[i]) {
      fprintf(stderr, "  bad signature\n");

      return 0;
    }
  }

  long pos = 8;
  int saw_ihdr = 0, saw_idat = 0, saw_iend = 0;
  static uint8_t zdata[1 << 20];
  size_t zlen = 0;
  while (pos + 12 <= size) {
    uint32_t len = rd32(fbuf + pos);
    const uint8_t *type = fbuf + pos + 4;
    if (pos + 12 + (long)len > size) {
      fprintf(stderr, "  chunk overruns file\n");

      return 0;
    }

    uint32_t crc = nl_crc32_update(0, type, 4 + len);
    if (crc != rd32(fbuf + pos + 8 + len)) {
      fprintf(stderr, "  bad chunk CRC\n");

      return 0;
    }

    if (type[0] == 'I' && type[1] == 'H') {
      const uint8_t *d = fbuf + pos + 8;
      if (len != 13 || rd32(d) != (uint32_t)w || rd32(d + 4) != (uint32_t)h ||
          d[8] != 8 || d[9] != (ch == 3 ? 2 : 0) || d[10] || d[11] || d[12]) {
        fprintf(stderr, "  bad IHDR\n");

        return 0;
      }

      saw_ihdr = 1;
    } else if (type[0] == 'I' && type[1] == 'D') {
      nl_memcpy(zdata + zlen, fbuf + pos + 8, len);
      zlen += len;
      saw_idat = 1;
    } else if (type[0] == 'I' && type[1] == 'E') {
      saw_iend = len == 0 && pos + 12 == size;
    }

    pos += 12 + len;
  }

  if (!saw_ihdr || !saw_idat || !saw_iend) {
    fprintf(stderr, "  missing chunk\n");

    return 0;
  }

  // zlib: header, stored blocks, adler
  if (zlen < 6 || zdata[0] != 0x78 || ((zdata[0] << 8) | zdata[1]) % 31 != 0) {
    fprintf(stderr, "  bad zlib header\n");

    return 0;
  }

  size_t zp = 2;
  static uint8_t raw[1 << 20];
  size_t rawn = 0;
  int final = 0;
  while (!final) {
    if (zp + 5 > zlen) {
      return 0;
    }

    final = zdata[zp] & 1;
    if ((zdata[zp] & 6) != 0) {
      fprintf(stderr, "  block not stored\n");

      return 0;
    }

    uint32_t n = zdata[zp + 1] | (zdata[zp + 2] << 8);
    uint32_t nn = zdata[zp + 3] | (zdata[zp + 4] << 8);
    if ((n ^ 0xFFFF) != nn || zp + 5 + n + 4 > zlen) {
      fprintf(stderr, "  bad stored block\n");

      return 0;
    }

    nl_memcpy(raw + rawn, zdata + zp + 5, n);
    rawn += n;
    zp += 5 + n;
  }

  if (zp + 4 != zlen || rd32(zdata + zp) != nl_adler32_update(1, raw, rawn)) {
    fprintf(stderr, "  bad adler or trailing bytes\n");

    return 0;
  }

  size_t rowlen = (size_t)w * ch;
  if (rawn != (rowlen + 1) * (size_t)h) {
    fprintf(stderr, "  raw size %zu\n", rawn);

    return 0;
  }

  for (int y = 0; y < h; y++) {
    const uint8_t *row = raw + (size_t)y * (rowlen + 1);
    if (row[0] != 0) {
      return 0;
    }

    for (size_t x = 0; x < rowlen; x++) {
      if (row[1 + x] != px[(size_t)y * rowlen + x]) {
        fprintf(stderr, "  pixel differs at row %d byte %zu\n", y, x);

        return 0;
      }
    }
  }

  return 1;
}

static uint8_t pic[400 * 300 * 3];

// Leftovers of an earlier (failed) run would shift -1/-2 names below
static void clean_stale(void) {
  static const char *stamps[] = {"20010909T014640Z", "20010909T014641Z",
                                 "20010909T014642Z", "20010909T014643Z"};
  static const char *exts[] = {".cast", ".png", ".ansi"};
  char p[REC_PATH_LEN];
  for (int a = 0; a < 4; a++) {
    for (int e = 0; e < 3; e++) {
      for (int i = 0; i < 4; i++) {
        if (i == 0) {
          nl_snprintf(p, sizeof(p), DIR "/asciicam-%s%s", stamps[a], exts[e]);
        } else {
          nl_snprintf(p, sizeof(p), DIR "/asciicam-%s-%d%s", stamps[a], i,
                      exts[e]);
        }

        unlink(p);
      }
    }
  }
}

int main(void) {
  rec_make_dirs(DIR);
  clean_stale();

  // ---- checksums ----
  CHECK(nl_crc32_update(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
  CHECK(nl_crc32_update(0, "", 0) == 0, "crc32 of nothing");
  uint32_t part = nl_crc32_update(0, "1234", 4);
  CHECK(nl_crc32_update(part, "56789", 5) == 0xCBF43926u, "crc32 chains");
  CHECK(nl_adler32_update(1, "Wikipedia", 9) == 0x11E60398u, "adler32 value");
  {
    static uint8_t big[100000];
    for (size_t i = 0; i < sizeof(big); i++) {
      big[i] = 0xFF;
    }

    // reference: run definition with 64-bit sums and a single reduction
    uint64_t a = 1, b = 0;
    for (size_t i = 0; i < sizeof(big); i++) {
      a = (a + big[i]) % 65521;
      b = (b + a) % 65521;
    }

    CHECK(nl_adler32_update(1, big, sizeof(big)) == (uint32_t)((b << 16) | a),
          "adler32 of 100000 x 0xFF (long-run reduction)");
  }

  // ---- timestamps ----
  char ts[17];
  rec_timestamp(ts, 0);
  CHECK(same(ts, "19700101T000000Z"), "epoch: %s", ts);
  rec_timestamp(ts, 1000000000);
  CHECK(same(ts, "20010909T014640Z"), "1e9: %s", ts);
  rec_timestamp(ts, 951782400);
  CHECK(same(ts, "20000229T000000Z"), "leap day 2000: %s", ts);
  rec_timestamp(ts, 951868799);
  CHECK(same(ts, "20000229T235959Z"), "end of leap day: %s", ts);
  rec_timestamp(ts, 1709164800);
  CHECK(same(ts, "20240229T000000Z"), "leap day 2024: %s", ts);
  rec_timestamp(ts, 4102444799);
  CHECK(same(ts, "20991231T235959Z"), "last second of 2099: %s", ts);
  rec_timestamp(ts, 4102444800);
  CHECK(same(ts, "21000101T000000Z"), "2100 (not a leap year): %s", ts);
  rec_timestamp(ts, 4107542400);
  CHECK(same(ts, "21000301T000000Z"), "2100-03-01: %s", ts);
  rec_timestamp(ts, 1793000000);
  CHECK(same(ts, "20261026T073320Z"), "2026-10-26: %s", ts);
  rec_timestamp(ts, -5);
  CHECK(same(ts, "19700101T000000Z"), "negative clamps: %s", ts);

  // ---- directory choice ----
  char d[64];
  CHECK(rec_default_dir(d, sizeof(d), "/home/u", "/data") == 0 &&
            same(d, "/data/asciicam/captures"),
        "xdg wins: %s", d);
  CHECK(rec_default_dir(d, sizeof(d), "/home/u", NULL) == 0 &&
            same(d, "/home/u/.local/share/asciicam/captures"),
        "home fallback: %s", d);
  CHECK(rec_default_dir(d, sizeof(d), "/home/u", "rel") == 0 &&
            same(d, "/home/u/.local/share/asciicam/captures"),
        "relative xdg ignored");
  CHECK(rec_default_dir(d, sizeof(d), NULL, NULL) == 0 && same(d, "captures"),
        "no env: cwd/captures");
  char small[10];
  CHECK(rec_default_dir(small, sizeof(small), "/home/u", NULL) == -1,
        "too small buffer refused");

  CHECK(rec_make_dirs(DIR "/a/b") == 0 && rec_make_dirs(DIR "/a/b") == 0,
        "mkdir -p twice");
  CHECK(rec_make_dirs("") == -1, "empty path refused");

  // ---- PNG ----
  for (int i = 0; i < (int)sizeof(pic); i++) {
    pic[i] = (uint8_t)(i * 31 + (i >> 8));
  }

  const char *p1 = DIR "/t1.png";
  CHECK(png_write(p1, 3, 2, 3, pic) == 0, "rgb 3x2 written");
  long sz = slurp(p1);
  CHECK(sz > 0 && png_verify(sz, 3, 2, 3, pic), "rgb 3x2 parses back");
  CHECK(!exists(DIR "/t1.png.tmp"), "no temp file left");

  CHECK(png_write(p1, 5, 4, 1, pic) == 0, "grey replaces existing file");
  sz = slurp(p1);
  CHECK(sz > 0 && png_verify(sz, 5, 4, 1, pic), "grey 5x4 parses back");

  CHECK(png_write(p1, 1, 1, 1, pic) == 0, "1x1");
  sz = slurp(p1);
  CHECK(sz > 0 && png_verify(sz, 1, 1, 1, pic), "1x1 parses back");

  // > 65535 raw bytes: several stored blocks, rows split across blocks
  CHECK(png_write(p1, 400, 300, 3, pic) == 0, "400x300 rgb written");
  sz = slurp(p1);
  CHECK(sz > 360000 && png_verify(sz, 400, 300, 3, pic),
        "multi-block png parses back");
  CHECK(png_write(p1, 400, 300, 1, pic) == 0, "400x300 grey written");
  sz = slurp(p1);
  CHECK(sz > 0 && png_verify(sz, 400, 300, 1, pic), "grey multi-block ok");

  CHECK(png_write(p1, 0, 5, 3, pic) == -1, "zero width refused");
  CHECK(png_write(p1, 5, -1, 3, pic) == -1, "negative height refused");
  CHECK(png_write(p1, 5, 5, 2, pic) == -1, "2 channels refused");
  CHECK(png_write(p1, 5, 5, 3, NULL) == -1, "NULL pixels refused");
  CHECK(png_write(p1, 20000, 5, 3, pic) == -1, "huge width refused");
  CHECK(png_write(DIR "/no/such/dir/x.png", 2, 2, 1, pic) == -1,
        "unwritable path fails");
  unlink(p1);

  // ---- recording ----
  recorder_t r;
  rec_init(&r);
  CHECK(rec_frame(&r, 0, "x", 1) == -1, "frame before start refused");
  CHECK(rec_stop(&r, 0, NULL) == -1, "stop before start refused");

  CHECK(rec_start(&r, DIR, 80, 24, 1000000000, 5000000000ull) == 0 &&
            r.active && r.fd >= 0,
        "start");
  CHECK(same(r.path, DIR "/asciicam-20010909T014640Z.cast"), "name: %s",
        r.path);
  char first_path[REC_PATH_LEN];
  nl_snprintf(first_path, sizeof(first_path), "%s", r.path);
  CHECK(rec_start(&r, DIR, 80, 24, 1000000000, 5000000000ull) == -1,
        "second start while active refused");

  const char *f1 = "\033[H\033[38;2;1;2;3m\"q\"\\ \xe2\xa0\x81\r\n";
  CHECK(rec_frame(&r, 5000000000ull, f1, nl_strlen(f1)) == 0, "frame 1");
  CHECK(rec_frame(&r, 5033333333ull, f1, nl_strlen(f1)) == 1,
        "identical frame skipped");
  CHECK(rec_frame(&r, 6500000000ull, "second", 6) == 0, "frame 2");
  CHECK(rec_frame(&r, 7000000000ull, "second", 6) == 1, "repeat skipped");
  CHECK(rec_frame(&r, 7250000000ull, f1, nl_strlen(f1)) == 0,
        "change then back is written");
  double secs = 0;
  CHECK(rec_stop(&r, 8000000000ull, &secs) == 3, "stop reports 3 frames");
  CHECK(secs > 2.99 && secs < 3.01, "length 3 s: %f", secs);
  CHECK(!r.active && r.fd == -1 && r.skipped == 2, "state after stop");

  sz = slurp(first_path);
  CHECK(sz > 0, "cast readable");
  const char *want_head =
      "{\"version\": 2, \"width\": 80, \"height\": 24, \"timestamp\": "
      "1000000000, \"env\": {\"TERM\": \"xterm-256color\"}}\n";
  CHECK(sz > 0 && has((const char *)fbuf, want_head), "header line");
  CHECK(has((const char *)fbuf,
            "[0.000000, \"o\", "
            "\"\\u001b[H\\u001b[38;2;1;2;3m\\\"q\\\"\\\\ \xe2\xa0\x81"
            "\\u000d\\u000a\"]\n"),
        "frame 1 escaped exactly");
  CHECK(has((const char *)fbuf, "[1.500000, \"o\", \"second\"]\n"),
        "frame 2 time 1.5 s");
  CHECK(has((const char *)fbuf, "[2.250000, \"o\", \"\\u001b[H"),
        "frame 3 time 2.25 s");
  CHECK(r.bytes == (uint64_t)sz, "byte counter matches file size");

  // same second: second file gets its own name, first is untouched
  CHECK(rec_start(&r, DIR, 100, 30, 1000000000, 0) == 0 &&
            same(r.path, DIR "/asciicam-20010909T014640Z-1.cast"),
        "same-second recording gets -1: %s", r.path);
  char second_path[REC_PATH_LEN];
  nl_snprintf(second_path, sizeof(second_path), "%s", r.path);
  CHECK(rec_stop(&r, 1000, NULL) == 0, "empty recording stops with 0 frames");
  sz = slurp(first_path);
  CHECK(sz > 0 && has((const char *)fbuf, "\"width\": 80"),
        "first recording not overwritten");

  CHECK(rec_start(&r, DIR "/no/such/\x01", 80, 24, 5, 0) == 0 || 1,
        "odd dir does not crash");
  if (r.active) {
    rec_stop(&r, 0, NULL);
    unlink(r.path);
  }

  CHECK(rec_start(&r, DIR, 0, 24, 5, 0) == -1, "zero width refused");
  CHECK(rec_start(&r, "/proc/nope", 80, 24, 5, 0) == -1,
        "unwritable directory refused");
  CHECK(!r.active, "failed start leaves recorder idle");

  // a long frame (bigger than internal buffer) round-trips
  {
    static char big[20000];
    for (int i = 0; i < (int)sizeof(big) - 1; i++) {
      big[i] = (char)('a' + i % 26);
    }

    big[sizeof(big) - 1] = '\0';
    CHECK(rec_start(&r, DIR, 80, 24, 1000000001, 0) == 0, "start long");
    CHECK(rec_frame(&r, 0, big, nl_strlen(big)) == 0, "long frame");
    char lp[REC_PATH_LEN];
    nl_snprintf(lp, sizeof(lp), "%s", r.path);
    rec_stop(&r, 10, NULL);
    sz = slurp(lp);
    CHECK(sz > 20000 && has((const char *)fbuf, big),
          "20000-byte frame stored whole");
    unlink(lp);
  }

  // ---- screenshot ----
  char png_path[REC_PATH_LEN], ansi_path[REC_PATH_LEN];
  CHECK(shot_save(DIR, 1000000002, pic, 4, 3, 3, "FRAME", 5, 24, png_path,
                  sizeof(png_path), ansi_path, sizeof(ansi_path)) == 0,
        "screenshot saved");
  CHECK(same(png_path, DIR "/asciicam-20010909T014642Z.png") &&
            same(ansi_path, DIR "/asciicam-20010909T014642Z.ansi"),
        "pair shares a stem: %s %s", png_path, ansi_path);
  sz = slurp(png_path);
  CHECK(sz > 0 && png_verify(sz, 4, 3, 3, pic), "screenshot png valid");
  sz = slurp(ansi_path);
  CHECK(sz > 0 && has((const char *)fbuf, "\033[2J\033[H"
                                          "FRAME"
                                          "\033[0m"),
        "ansi: clear, frame, reset");
  CHECK(has((const char *)fbuf, "\033[24;1H"), "ansi parks cursor at last row");

  char png2[REC_PATH_LEN], ansi2[REC_PATH_LEN];
  CHECK(shot_save(DIR, 1000000002, NULL, 0, 0, 0, "X", 1, 24, png2,
                  sizeof(png2), ansi2, sizeof(ansi2)) == 0,
        "ansi-only screenshot");
  CHECK(png2[0] == '\0' && same(ansi2, DIR "/asciicam-20010909T014642Z-1.ansi"),
        "no png path, ansi did not overwrite: %s", ansi2);
  CHECK(shot_save("/proc/nope", 5, pic, 2, 2, 3, "X", 1, 24, png2, sizeof(png2),
                  ansi2, sizeof(ansi2)) == -1 &&
            png2[0] == '\0' && ansi2[0] == '\0',
        "unwritable dir: -1, empty paths");
  CHECK(shot_save(DIR, 1000000003, pic, 0, 0, 3, "X", 1, 24, png2, sizeof(png2),
                  ansi2, sizeof(ansi2)) == -1 &&
            ansi2[0] != '\0' && png2[0] == '\0',
        "bad picture: -1 but ansi kept");

  char clk[16];
  rec_clock(clk, 65.9);
  CHECK(same(clk, "1:05"), "clock 65.9 s: %s", clk);
  rec_clock(clk, -3);
  CHECK(same(clk, "0:00"), "negative clock");
  rec_clock(clk, 3725);
  CHECK(same(clk, "62:05"), "long clock: %s", clk);

  // clean up what is left
  unlink(first_path);
  unlink(second_path);
  unlink(png_path);
  unlink(ansi_path);
  unlink(ansi2);
  unlink(DIR "/asciicam-20010909T014642Z-1.ansi");
  {
    char extra[REC_PATH_LEN];
    nl_snprintf(extra, sizeof(extra), DIR "/asciicam-20010909T014643Z.ansi");
    unlink(extra);
    nl_snprintf(extra, sizeof(extra), DIR "/asciicam-20010909T014643Z.png");
    unlink(extra);
  }

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

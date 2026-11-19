/*
 * tests/test_mic.c - mic.c: device names, sample conversion, kernel ABI layout
 * and error paths that need no sound card
 */

#include "mic.h"
#include "nolibc.h"

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

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

static void test_paths(void) {
  char p[128];

  CHECK(mic_device_path("default", p, sizeof p) == 0 &&
            streq(p, "/dev/snd/pcmC0D0c"),
        "default -> %s", p);
  CHECK(mic_device_path("hw:1,3", p, sizeof p) == 0 &&
            streq(p, "/dev/snd/pcmC1D3c"),
        "hw:1,3 -> %s", p);
  CHECK(mic_device_path("hw:12,10", p, sizeof p) == 0 &&
            streq(p, "/dev/snd/pcmC12D10c"),
        "hw:12,10 -> %s", p);
  CHECK(mic_device_path("/dev/snd/pcmC2D0c", p, sizeof p) == 0 &&
            streq(p, "/dev/snd/pcmC2D0c"),
        "absolute path kept");
  CHECK(mic_device_path("hw:1", p, sizeof p) != 0, "hw:1 rejected");
  CHECK(mic_device_path("hw:,0", p, sizeof p) != 0, "hw:,0 rejected");
  CHECK(mic_device_path("hw:1,0x", p, sizeof p) != 0, "junk suffix rejected");
  CHECK(mic_device_path("pulse", p, sizeof p) != 0, "names rejected");
  CHECK(mic_device_path("hw:0,0", p, 8) != 0, "too small buffer rejected");
}

static void test_convert(void) {
  int16_t out[16];

  const int16_t mono[4] = {1, -2, 3, 4};
  CHECK(mic_convert(mono, 4, 1, 1, out) == 4 && out[1] == -2 && out[3] == 4,
        "mono passthrough");

  const int16_t st[6] = {2, 4, 6, 8, -3, 3};
  CHECK(mic_convert(st, 3, 2, 1, out) == 3 && out[0] == 3 && out[1] == 7 &&
            out[2] == 0,
        "stereo averaged to mono");

  const int16_t wide[7] = {3, 6, 9, 1, 1, 1, 100};
  size_t n = mic_convert(wide, 7, 1, 3, out);
  CHECK(n == 2 && out[0] == 6 && out[1] == 1,
        "decimate by 3 box-averages, drops the tail (n=%u)", (unsigned)n);

  const int16_t wide_st[6] = {2, 2, 4, 4, 6, 6};
  n = mic_convert(wide_st, 3, 2, 3, out);
  CHECK(n == 1 && out[0] == 4, "stereo decimate");

  CHECK(mic_convert(mono, 4, 3, 1, out) == 0, "bad channel count refused");
  CHECK(mic_convert(mono, 4, 1, 0, out) == 0, "bad decimation refused");
}

#if defined(PLATFORM_LINUX) && defined(__LP64__)
// These are what sound/asound.h gives on 64-bit Linux
static void test_abi(void) {
  CHECK(sizeof(mic_mask_t) == 32, "mask size %u", (unsigned)sizeof(mic_mask_t));
  CHECK(sizeof(mic_interval_t) == 12, "interval size");
  CHECK(sizeof(mic_hw_params_t) == 608, "hw_params size %u",
        (unsigned)sizeof(mic_hw_params_t));
  CHECK(MIC_IOC_HW_PARAMS == 0xc2604111ul, "HW_PARAMS number %lx",
        MIC_IOC_HW_PARAMS);
  CHECK(MIC_IOC_PREPARE == 0x4140ul, "PREPARE number");
  CHECK(MIC_IOC_START == 0x4142ul, "START number");
  CHECK(MIC_IOC_DROP == 0x4143ul, "DROP number");
  CHECK(MIC_IVAL(MIC_HW_CHANNELS) == 2 && MIC_IVAL(MIC_HW_RATE) == 3,
        "interval indices");
}
#endif

static void test_open_errors(void) {
  mic_t m;
  char err[96];

  err[0] = 0;
  CHECK(mic_open(&m, "/nonexistent/snd/pcmC9D9c", err, sizeof err) != 0,
        "missing device fails");
  CHECK(err[0] != 0, "missing device gives a reason");
  CHECK(mic_read(&m, (int16_t[4]){0}, 4) < 0, "read after failed open");

  CHECK(mic_open(&m, "not a device", err, sizeof err) != 0, "bad name fails");
  mic_close(&m);
  mic_close(&m);
  CHECK(m.fd == -1, "close is idempotent");
}

int main(void) {
  test_paths();
  test_convert();
#if defined(PLATFORM_LINUX) && defined(__LP64__)
  test_abi();
#endif
  test_open_errors();

  fprintf(stderr, "mic: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n != 0;
}

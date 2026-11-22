/*
 * tests/measure_bandwidth.c - bytes per frame program writes to terminal, with
 * and without --net
 *
 *   cc -O1 -o build/tests/measure_bandwidth tests/measure_bandwidth.c
 *   build/tests/measure_bandwidth build/asciicam [seconds]
 *
 * Runs demo source in a pseudo-terminal for each render/colour mode and --net
 * setting and prints bytes per frame and kbit/s (frames are counted by
 * synchronized-update markers)
 * Panel and overlays are included, so figures are what actually crosses wire
 */

#include "pty_harness.h"

#include <stdio.h>

static const char *BIN;
static double SECS = 4.0;

// Run program for `secs` in a rows x cols terminal, then press q
// Fills `out` with everything it printed
static void run(const char *const *args, int nargs, double secs, pty_buf *out) {
  char *argv[32];
  int ac = 0;
  argv[ac++] = (char *)BIN;
  for (int i = 0; i < nargs && ac < 30; i++) {
    argv[ac++] = (char *)args[i];
  }

  argv[ac] = NULL;

  pty_proc p;
  if (pty_spawn(&p, BIN, argv, 40, 100) != 0) {
    fprintf(stderr, "cannot start %s in a pty\n", BIN);
    exit(1);
  }

  double t0 = pty_now();
  int quit_sent = 0;
  while (pty_now() - t0 < secs + 4) {
    if (pty_read(&p, out, 20) < 0) {
      break;
    }

    if (!quit_sent && pty_now() - t0 > secs) {
      pty_write(&p, "q", 1);
      quit_sent = 1;
    }
  }

  pty_finish(&p, 2.0);
}

typedef struct {
  const char *name;
  const char *args[4];
  int n;
} setting_t;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <asciicam-binary> [seconds]\n", argv[0]);

    return 2;
  }

  BIN = argv[1];
  if (access(BIN, X_OK) != 0) {
    fprintf(stderr, "%s is not an executable\n", BIN);

    return 2;
  }

  if (argc > 2) {
    SECS = atof(argv[2]);
  }

  static const setting_t modes[] = {
      {"ascii mono", {"-m", "ascii"}, 2},
      {"ascii 256-colour", {"-m", "ascii", "-2"}, 3},
      {"ascii truecolor", {"-m", "ascii", "-C"}, 3},
      {"halfblock truecolor", {"-m", "halfblock", "-C"}, 3},
  };
  static const setting_t nets[] = {
      {"off", {0}, 0},
      {"--net 0", {"--net", "0"}, 2},
      {"--net 1000", {"--net", "1000"}, 2},
      {"--net 300", {"--net", "300"}, 2},
      {"--net 100", {"--net", "100"}, 2},
  };

  printf("%-20s%-12s%9s%9s\n", "mode", "setting", "B/frame", "kbit/s");
  for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
    for (size_t n = 0; n < sizeof nets / sizeof nets[0]; n++) {
      const char *args[16];
      int na = 0;
      args[na++] = "--demo";
      args[na++] = "-r";
      args[na++] = "20";
      for (int i = 0; i < modes[m].n; i++) {
        args[na++] = modes[m].args[i];
      }

      for (int i = 0; i < nets[n].n; i++) {
        args[na++] = nets[n].args[i];
      }

      pty_buf out = {0};
      run(args, na, SECS, &out);
      size_t frames = pty_buf_count(&out, "\x1b[?2026h");
      if (frames < 1) {
        frames = 1;
      }

      printf("%-20s%-12s%9zu%9.0f\n", modes[m].name, nets[n].name,
             out.len / frames, (double)out.len * 8 / SECS / 1000);
      fflush(stdout);
      free(out.data);
    }
  }

  return 0;
}

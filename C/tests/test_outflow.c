/*
 * tests/test_optflow.c - filters/optflow.c against a known synthetic motion
 *
 * A smooth texture is shifted by an exact (dx, dy) between two frames; each
 * trusted interior cell must match it within half a pixel
 * Limit: this is a single-scale estimator, so it is accurate for motion of
 * about 2 px per frame or less; faster motion needs a coarse-to-fine pyramid
 * (not here yet) Controls: a static scene must give zero flow, a flat wall must
 * give no trusted cell at all and opposite shift must give opposite flow
 * Then plugin as a whole: pass-through at parameter 0, and a moving scene
 * changes picture while a still one does not
 */

#define TESTING
#include "../filters/optflow.c"

static int pass_n, fail_n;

#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      if (fail_n < 30) {                                                       \
        fprintf(stderr, "FAIL line %d: ", __LINE__);                           \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                 \
      }                                                                        \
    }                                                                          \
  } while (0)

enum { W = 96, H = 80 };
static uint8_t scene_prev[W * H], scene_cur[W * H], tmp_out[W * H];

// Smooth texture, about 60 levels of contrast, no flat areas
static int texture(int x, int y) {
  return 128 + (60 * fx_sin(x * 12 + y * 5)) / 256 +
         (40 * fx_sin(x * 3 - y * 9 + 300)) / 256;
}

// Content moves by (dx, dy): previous frame shows what is now at x + dx
static void make_pair(int dx, int dy) {
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      scene_cur[y * W + x] = fx_u8(texture(x, y));
      scene_prev[y * W + x] = fx_u8(texture(x + dx, y + dy));
    }
  }
}

static void reset_state(void) {
  of_w = of_h = 0;
  of_cw = of_ch = 0;
  nl_memset(of_ok, 0, sizeof(of_ok));
}

// Runs estimator on a prev/cur pair with fresh grid state
static void run_flow(const uint8_t *prev, const uint8_t *cur) {
  of_w = W;
  of_h = H;
  of_cw = (W + OF_CELL - 1) / OF_CELL;
  of_ch = (H + OF_CELL - 1) / OF_CELL;
  of_estimate(prev, cur, W, H);
}

static int near(int got, int want, int tol) {
  return got >= want - tol && got <= want + tol;
}

// Two properties, on interior cells only (borders see clamped pixels):
//  - accuracy: every trusted cell is within half a pixel of true motion
//  - coverage: texture is strong enough that most cells are trusted
static void check_shift(int dx, int dy, const char *name) {
  make_pair(dx, dy);
  run_flow(scene_prev, scene_cur);

  int cw = of_cw, ch = of_ch, trusted = 0, total = 0, wrong = 0;
  for (int cy = 1; cy < ch - 1; cy++) {
    for (int cx = 1; cx < cw - 1; cx++) {
      int i = cy * cw + cx;
      total++;
      if (!of_ok[i]) {
        continue;
      }
      trusted++;
      if (!near(of_fu[i], dx * OF_SUBPX, OF_SUBPX / 2) ||
          !near(of_fv[i], dy * OF_SUBPX, OF_SUBPX / 2)) {
        wrong++;
      }
    }
  }
  CHECK(total > 0 && wrong == 0,
        "%s: %d trusted cells, %d off by more than half a pixel", name, trusted,
        wrong);
  CHECK(trusted * 10 >= total * 6, "%s: only %d of %d cells trusted", name,
        trusted, total);
}

int main(void) {
  // Known motions, both signs and both axes
  check_shift(2, 1, "right-down");
  check_shift(-2, 0, "left");
  check_shift(0, -2, "up");
  check_shift(1, 1, "diagonal");

  // Control: nothing moves, so flow is zero everywhere it is trusted
  make_pair(0, 0);
  run_flow(scene_prev, scene_cur);
  int still_bad = 0;
  for (int i = 0; i < of_cw * of_ch; i++) {
    if (of_ok[i] && (fx_abs(of_fu[i]) > 1 || fx_abs(of_fv[i]) > 1)) {
      still_bad++;
    }
  }
  CHECK(still_bad == 0, "static scene gives zero flow");

  // Control: flat wall has no texture, so no cell may be trusted
  for (int i = 0; i < W * H; i++) {
    scene_prev[i] = scene_cur[i] = 90;
  }
  run_flow(scene_prev, scene_cur);
  int trusted = 0;
  for (int i = 0; i < of_cw * of_ch; i++) {
    trusted += of_ok[i];
  }
  CHECK(trusted == 0, "flat wall: %d cells trusted, want 0", trusted);

  // Control: a single straight edge has aperture problem, only normal
  // component is known; eigenvalue test must reject it
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      scene_prev[y * W + x] = (uint8_t)(x < 40 ? 40 : 200);
      scene_cur[y * W + x] = (uint8_t)(x < 42 ? 40 : 200);
    }
  }
  run_flow(scene_prev, scene_cur);
  int edge_trusted = 0;
  for (int cy = 1; cy < of_ch - 1; cy++) {
    for (int cx = 1; cx < of_cw - 1; cx++) {
      edge_trusted += of_ok[cy * of_cw + cx];
    }
  }
  CHECK(edge_trusted == 0, "pure edge: %d interior cells trusted, want 0",
        edge_trusted);

  // Plugin level: parameter 0 leaves picture alone
  reset_state();
  int zero = 0;
  make_pair(2, 1);
  nl_memcpy(tmp_out, scene_cur, W * H);
  optflow_process(scene_prev, W, H, &zero);
  optflow_process(tmp_out, W, H, &zero);
  int same = 1;
  for (int i = 0; i < W * H; i++) {
    if (tmp_out[i] != scene_cur[i]) {
      same = 0;
    }
  }
  CHECK(same, "parameter 0 passes the frame through");

  // Plugin level: moving scene is drawn, still scene is only dimmed by a
  // uniform amount with no strokes (every pixel of a still frame is either
  // dimmed value or dimmed value with same offset)
  int param = 128;
  reset_state();
  make_pair(0, 0);
  nl_memcpy(scene_prev, scene_cur, W * H);
  optflow_process(scene_prev, W, H, &param); // first frame: init
  nl_memcpy(tmp_out, scene_cur, W * H);
  optflow_process(tmp_out, W, H, &param);
  int strokes_still = 0;
  for (int i = 0; i < W * H; i++) {
    if (tmp_out[i] == 255) {
      strokes_still++;
    }
  }
  CHECK(strokes_still == 0, "still scene draws no strokes (%d pixels)",
        strokes_still);

  reset_state();
  make_pair(2, 1);
  optflow_process(scene_prev, W, H, &param); // first frame: init
  nl_memcpy(tmp_out, scene_cur, W * H);
  optflow_process(tmp_out, W, H, &param);
  int strokes_move = 0;
  for (int i = 0; i < W * H; i++) {
    if (tmp_out[i] == 255) {
      strokes_move++;
    }
  }
  CHECK(strokes_move > 20, "moving scene draws strokes (%d pixels)",
        strokes_move);

  fprintf(stderr, "optflow: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}

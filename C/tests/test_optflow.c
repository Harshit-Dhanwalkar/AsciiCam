/*
 * tests/test_optflow.c - filters/optflow.c against a known synthetic motion
 *
 * A smooth texture is shifted by an exact (dx, dy) between two frames. Each
 * trusted interior cell must match the true motion within half a pixel. The
 * shifts go up to 6 px, which single-scale Lucas-Kanade cannot follow; the
 * pyramid is what makes them pass.
 * Controls: a static scene must give zero flow, a flat wall must give no
 * trusted cell at all, a pure edge must give none either, and the opposite
 * shift must give the opposite flow
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
static uint8_t scene_prev[W * H];
static uint8_t scene_cur[W * H];
static uint8_t tmp_out[W * H];

// Smooth texture with about 60 levels of contrast, no flat areas
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

static void reset_state(void) { of_w = of_h = 0; }

static int near(int got, int want, int tol) {
  return got >= want - tol && got <= want + tol;
}

static int cells_w(void) { return (W + OF_CELL - 1) / OF_CELL; }
static int cells_h(void) { return (H + OF_CELL - 1) / OF_CELL; }

// Two properties, on interior cells only (borders see clamped pixels):
//  - accuracy: every trusted cell is within half a pixel of true motion
//  - coverage: texture is strong enough that most cells are trusted
static void check_shift(int dx, int dy, const char *name) {
  make_pair(dx, dy);
  of_flow(scene_prev, scene_cur, W, H);

  int cw = cells_w();
  int ch = cells_h();
  int trusted = 0;
  int total = 0;
  int wrong = 0;
  for (int cy = 1; cy < ch - 1; cy++) {
    for (int cx = 1; cx < cw - 1; cx++) {
      int i = cy * cw + cx;
      total++;
      if (!of_lok[0][i]) {
        continue;
      }
      trusted++;
      if (!near(of_lu[0][i], dx * OF_SUBPX, OF_SUBPX / 2) ||
          !near(of_lv[0][i], dy * OF_SUBPX, OF_SUBPX / 2)) {
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
  // Known motions: small, both signs, both axes, then past single-scale range
  check_shift(2, 1, "right-down 2px");
  check_shift(-2, 0, "left 2px");
  check_shift(0, -2, "up 2px");
  check_shift(1, 1, "diagonal 1px");
  check_shift(6, 2, "right 6px");
  check_shift(-5, 0, "left 5px");
  check_shift(0, -4, "up 4px");
  check_shift(3, 3, "diagonal 3px");

  // Control: nothing moves, so flow is zero everywhere it is trusted
  make_pair(0, 0);
  of_flow(scene_prev, scene_cur, W, H);
  int still_bad = 0;
  for (int i = 0; i < cells_w() * cells_h(); i++) {
    if (of_lok[0][i] && (fx_abs(of_lu[0][i]) > 1 || fx_abs(of_lv[0][i]) > 1)) {
      still_bad++;
    }
  }
  CHECK(still_bad == 0, "static scene gives zero flow");

  // Control: flat wall has no texture, so no cell may be trusted
  for (int i = 0; i < W * H; i++) {
    scene_prev[i] = scene_cur[i] = 90;
  }
  of_flow(scene_prev, scene_cur, W, H);
  int trusted = 0;
  for (int i = 0; i < cells_w() * cells_h(); i++) {
    trusted += of_lok[0][i];
  }
  CHECK(trusted == 0, "flat wall: %d cells trusted, want 0", trusted);

  // Control: a single straight edge has aperture problem; eigenvalue test must
  // reject it
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      scene_prev[y * W + x] = (uint8_t)(x < 40 ? 40 : 200);
      scene_cur[y * W + x] = (uint8_t)(x < 42 ? 40 : 200);
    }
  }
  of_flow(scene_prev, scene_cur, W, H);
  int edge_trusted = 0;
  for (int cy = 1; cy < cells_h() - 1; cy++) {
    for (int cx = 1; cx < cells_w() - 1; cx++) {
      edge_trusted += of_lok[0][cy * cells_w() + cx];
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

  // Plugin level: a still scene draws no strokes; a moving one does
  int param = 128;
  reset_state();
  make_pair(0, 0);
  optflow_process(scene_prev, W, H, &param); // First frame: init
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

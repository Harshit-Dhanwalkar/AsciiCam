#include "nolibc.h"

#include "fx.h"
#include "plugins.h"

/* dither_fs - Floyd-Steinberg error diffusion, run on blocks of (width / 160)
 * px so the grain survives the ASCII conversion
 * Parameter: number of grey levels, 2 (p=0) .. 17 (p=255); 128 gives 10 */

#define FS_GRID (FX_MAX_SIDE + 2)

static int fs_err[2][FS_GRID];

void dither_fs_process(uint8_t *gray, int w, int h, void *ctx) {
  if (!fx_ok(gray, w, h)) {
    return;
  }

  int p = fx_param(ctx, 128);
  int levels = 2 + p / 16;
  int step = 255 / (levels - 1);
  int cell = w / 160 > 1 ? w / 160 : 1;
  int gw = (w + cell - 1) / cell;
  int gh = (h + cell - 1) / cell;

  nl_memset(fs_err, 0, sizeof(fs_err));
  int cur = 0;

  for (int gy = 0; gy < gh; gy++) {
    int *e = fs_err[cur];
    int *next = fs_err[1 - cur];
    int y0 = gy * cell;
    int y1 = y0 + cell < h ? y0 + cell : h;

    for (int gx = 0; gx < gw; gx++) {
      int x0 = gx * cell;
      int x1 = x0 + cell < w ? x0 + cell : w;

      int sum = 0;
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          sum += gray[y * w + x];
        }
      }

      int v = sum / ((y1 - y0) * (x1 - x0)) + e[gx + 1] / 16;
      int q = fx_clamp((v + step / 2) / step, 0, levels - 1) * step;
      int err = v - q;
      e[gx + 2] += err * 7;
      next[gx] += err * 3;
      next[gx + 1] += err * 5;
      next[gx + 2] += err;

      uint8_t out = fx_u8(q);
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          gray[y * w + x] = out;
        }
      }
    }

    // this row is done: it becomes scratch space for the one after next
    nl_memset(e, 0, sizeof(fs_err[0]));
    cur = 1 - cur;
  }
}

#ifndef TESTING
static filter_plugin_t self = {.process = dither_fs_process,
                               .name = "dither_fs"};

filter_plugin_t *plugin_get(void) { return &self; }

PLUGIN_ABI_EXPORT;
#endif

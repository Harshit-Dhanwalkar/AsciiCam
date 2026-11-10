/*
 * src/curve.c - tone curve, see include/curve.h
 */

#include "platform.h"

#include "nolibc.h"

#include "curve.h"

typedef struct {
  const char *name;
  int n;
  curve_pt_t p[CURVE_MAX_POINTS];
} preset_t;

// \asinh and \sqrt are sampled from \asinh(10 x) / \asinh(10) and \sqrt(x)
static const preset_t presets[] = {
    {"linear", 2, {{0, 0}, {255, 255}}},
    {"s-curve", 4, {{0, 0}, {64, 46}, {192, 209}, {255, 255}}},
    {"asinh",
     6,
     {{0, 0}, {16, 50}, {64, 142}, {128, 197}, {192, 231}, {255, 255}}},
    {"sqrt",
     6,
     {{0, 0}, {16, 64}, {64, 128}, {128, 181}, {192, 221}, {255, 255}}},
    {"invert", 2, {{0, 255}, {255, 0}}},
    {"threshold", 4, {{0, 0}, {120, 0}, {136, 255}, {255, 255}}},
    {"solarize", 3, {{0, 0}, {128, 255}, {255, 0}}},
};

#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

int curve_preset_count(void) { return NPRESETS; }

const char *curve_preset_name(int i) {
  return i >= 0 && i < NPRESETS ? presets[i].name : "custom";
}

static uint8_t clamp8(double v) {
  if (v <= 0.0) {
    return 0;
  }
  if (v >= 255.0) {
    return 255;
  }

  return (uint8_t)(v + 0.5);
}

void curve_build(curve_t *c) {
  int n = c->n;
  double d[CURVE_MAX_POINTS], m[CURVE_MAX_POINTS];
  for (int k = 0; k + 1 < n; k++) {
    d[k] = ((double)c->p[k + 1].y - (double)c->p[k].y) /
           ((double)c->p[k + 1].x - (double)c->p[k].x);
  }

  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (int k = 1; k + 1 < n; k++) {
    if (d[k - 1] * d[k] <= 0.0) {
      m[k] = 0.0; // turning point or flat: stay flat there
    } else {
      double a = d[k - 1] < 0 ? -d[k - 1] : d[k - 1];
      double b = d[k] < 0 ? -d[k] : d[k];
      double lim = 3.0 * (a < b ? a : b);
      double v = (d[k - 1] + d[k]) / 2.0;
      double av = v < 0 ? -v : v;
      m[k] = av > lim ? (v < 0 ? -lim : lim) : v;
    }
  }

  for (int k = 0; k + 1 < n; k++) {
    double h = (double)c->p[k + 1].x - (double)c->p[k].x;
    double y0 = c->p[k].y, y1 = c->p[k + 1].y;
    for (int x = c->p[k].x; x < c->p[k + 1].x; x++) {
      double t = ((double)x - (double)c->p[k].x) / h;
      double t2 = t * t, t3 = t2 * t;
      double v = (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m[k] +
                 (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * h * m[k + 1];
      c->lut[x] = clamp8(v);
    }
  }

  c->lut[255] = c->p[n - 1].y;
  c->identity = 1;
  for (int i = 0; i < 256; i++) {
    if (c->lut[i] != i) {
      c->identity = 0;
      break;
    }
  }
}

void curve_set_preset(curve_t *c, int i) {
  if (i < 0 || i >= NPRESETS) {
    return;
  }

  c->n = presets[i].n;
  for (int k = 0; k < c->n; k++) {
    c->p[k] = presets[i].p[k];
  }

  c->preset = i;
  curve_build(c);
}

void curve_reset(curve_t *c) {
  c->enabled = 1;
  curve_set_preset(c, CURVE_PRESET_LINEAR);
}

int curve_active(const curve_t *c) { return c->enabled && !c->identity; }

int curve_add(curve_t *c, int x, int y) {
  if (c->n >= CURVE_MAX_POINTS || x < 1 || x > 254) {
    return -1;
  }

  int at = 0;
  while (at < c->n && c->p[at].x < x) {
    at++;
  }

  if (at < c->n && c->p[at].x == x) {
    return -1;
  }

  for (int k = c->n; k > at; k--) {
    c->p[k] = c->p[k - 1];
  }

  c->p[at].x = (uint8_t)x;
  c->p[at].y = clamp8((double)y);
  c->n++;
  c->preset = -1;
  curve_build(c);

  return at;
}

int curve_remove(curve_t *c, int idx) {
  if (idx <= 0 || idx >= c->n - 1) {
    return -1;
  }

  for (int k = idx; k + 1 < c->n; k++) {
    c->p[k] = c->p[k + 1];
  }

  c->n--;
  c->preset = -1;
  curve_build(c);

  return 0;
}

int curve_move(curve_t *c, int idx, int x, int y) {
  if (idx < 0 || idx >= c->n) {
    return -1;
  }

  if (idx > 0 && idx < c->n - 1) {
    int lo = c->p[idx - 1].x + 1, hi = c->p[idx + 1].x - 1;
    x = x < lo ? lo : x > hi ? hi : x;
    c->p[idx].x = (uint8_t)x;
  }

  c->p[idx].y = clamp8((double)y);
  c->preset = -1;
  curve_build(c);

  return 0;
}

int curve_nearest(const curve_t *c, int x) {
  int best = -1, bd = 1000;
  for (int k = 0; k < c->n; k++) {
    int d = c->p[k].x - x;
    d = d < 0 ? -d : d;
    if (d < bd) {
      bd = d;
      best = k;
    }
  }

  return best;
}

static void set_err(char *err, size_t errsz, const char *msg) {
  if (err && errsz) {
    nl_snprintf(err, errsz, "%s", msg);
  }
}

int curve_parse(curve_t *c, const char *spec, char *err, size_t errsz) {
  while (*spec == ' ') {
    spec++;
  }

  for (int i = 0; i < NPRESETS; i++) {
    if (nl_strcmp(spec, presets[i].name) == 0) {
      curve_set_preset(c, i);

      return 0;
    }
  }

  curve_t t = *c;
  t.n = 0;
  const char *s = spec;
  while (*s) {
    if (*s == ' ' || *s == ';') {
      s++;

      continue;
    }

    int v[2] = {0, 0};
    for (int f = 0; f < 2; f++) {
      if (*s < '0' || *s > '9') {
        set_err(err, errsz,
                "curve: expected x,y pairs like 0,0 128,160 255,255");

        return -1;
      }

      while (*s >= '0' && *s <= '9') {
        v[f] = v[f] * 10 + (*s++ - '0');
        if (v[f] > 255) {
          set_err(err, errsz, "curve: values are 0..255");

          return -1;
        }
      }

      if (f == 0) {
        if (*s != ',') {
          set_err(err, errsz,
                  "curve: expected x,y pairs like 0,0 128,160 255,255");

          return -1;
        }

        s++;
      }
    }

    if (*s && *s != ' ' && *s != ';') {
      set_err(err, errsz, "curve: expected x,y pairs like 0,0 128,160 255,255");

      return -1;
    }

    if (t.n >= CURVE_MAX_POINTS) {
      set_err(err, errsz, "curve: at most 8 points");

      return -1;
    }

    if (t.n > 0 && v[0] <= t.p[t.n - 1].x) {
      set_err(err, errsz, "curve: x must increase from point to point");

      return -1;
    }

    t.p[t.n].x = (uint8_t)v[0];
    t.p[t.n].y = (uint8_t)v[1];
    t.n++;
  }

  if (t.n < 2 || t.p[0].x != 0 || t.p[t.n - 1].x != 255) {
    set_err(err, errsz,
            "curve: needs a first point at x=0 and a last at x=255");

    return -1;
  }

  t.preset = -1;
  curve_build(&t);
  *c = t;

  return 0;
}

int curve_format(const curve_t *c, char *out, size_t outsz) {
  int pos = 0;
  if (outsz == 0) {
    return 0;
  }

  out[0] = '\0';
  for (int k = 0; k < c->n; k++) {
    int w = nl_snprintf(out + pos, outsz - (size_t)pos, "%s%d,%d", k ? " " : "",
                        c->p[k].x, c->p[k].y);
    if (w < 0 || (size_t)w >= outsz - (size_t)pos) {
      break;
    }

    pos += w;
  }

  return pos;
}

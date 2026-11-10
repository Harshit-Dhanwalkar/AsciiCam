#ifndef CURVE_H
#define CURVE_H

/*
 * Tone curve: maps brightness of every cell (0..255) to a new brightness
 * through a smooth curve that goes through up to CURVE_MAX_POINTS control
 * points
 *
 * Curve is a monotone cubic Hermite spline (Fritsch-Carlson with Hyman's
 * limiter): it never overshoots between points, so a curve through a few rising
 * points always stays rising, no sqrt or libm needed. A curve whose points go
 * up and down (solarize) stays smooth and flat at its turning points
 *
 * First point is always at x = 0 and last at x = 255, only their y can change
 * (swap them to invert)
 */

#include "nl_types.h"

#define CURVE_MAX_POINTS 8
#define CURVE_PRESET_LINEAR 0

typedef struct {
  uint8_t x, y;
} curve_pt_t;

typedef struct {
  int n; // 2..CURVE_MAX_POINTS
  curve_pt_t p[CURVE_MAX_POINTS];
  uint8_t lut[256]; // built from p by curve_build
  int identity;     // lut[i] == i for all i
  int enabled;      // 0 = bypass (curve kept, not applied)
  int preset;       // index of preset points came from, -1 = edited
} curve_t;

// Linear curve, enabled
void curve_reset(curve_t *c);

int curve_preset_count(void);
const char *curve_preset_name(int i);
// Replace points with preset i (0 = linear)
// Out of range: no change
void curve_set_preset(curve_t *c, int i);

// Rebuild lut and identity from points
// All functions below that change points call it, so lut is always current
void curve_build(curve_t *c);

// Apply curve when true: enabled and not a straight line
int curve_active(const curve_t *c);

// Add a point (1 <= x <= 254, no point at x yet, room left)
// Returns its index, or -1
int curve_add(curve_t *c, int x, int y);

// Delete an inner point. 0, or -1 for an end point / bad index
int curve_remove(curve_t *c, int idx);

// Move point idx to (x, y): y is clamped to 0..255, x to gap between its
// neighbours (end points keep x)
// Returns 0, or -1 for a bad index
int curve_move(curve_t *c, int idx, int x, int y);

// Point nearest to x in 0..255, by x distance only
// Returns -1 when none
int curve_nearest(const curve_t *c, int x);

// "x,y x,y ..." (space or ';' separated) or name of a preset
// Returns 0 on success; -1 and a message in err otherwise (c untouched)
int curve_parse(curve_t *c, const char *spec, char *err, size_t errsz);

// Points as "x,y x,y ..." for curve_parse / --curve
// Bytes written
int curve_format(const curve_t *c, char *out, size_t outsz);

#endif

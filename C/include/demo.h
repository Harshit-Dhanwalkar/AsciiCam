#ifndef DEMO_H
#define DEMO_H

#include "nl_types.h"

/*
 * Built-in test picture for `--demo` / `-d demo`: an animated plasma, so
 * program can be tried (and screenshotted, and run in a container) without a
 * webcam. Pure function of frame number, no state, no I/O
 *
 * Fills a YUYV frame of w x h pixels (w even) whose rows are `stride` bytes
 * apart. t is frame number; picture loops every DEMO_PERIOD frames
 */
#define DEMO_DEVICE "demo"
#define DEMO_PERIOD 256

void demo_fill_yuyv(uint8_t *yuyv, int w, int h, int stride, long t);

// 1 if device names the built-in source
int demo_is_device(const char *device);

#endif

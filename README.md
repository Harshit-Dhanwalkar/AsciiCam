# AsciiCam

Real-time ASCII video from your webcam in the terminal - pure C99, no heavy runtime dependencies.

<img src="assets/demo.gif" width="325">

### Edge detection + threshold plugin

<img src="assets/demo-edgedetection.gif" width="325">

---

## Features

| Feature                   | Details                                                                               |
| ------------------------- | ------------------------------------------------------------------------------------- |
| YUYV to grayscale          | SSE2 SIMD (`yuyv_to_gray_simd`) - 16 pixels per iteration                             |
| YUYV to RGB                | Fixed-point BT.601 conversion for truecolor output                                    |
| ASCII rendering           | Configurable charset, brightness, contrast, invert                                    |
| Sobel edge detection      | L1-norm kernel convolution                                                            |
| Floyd-Steinberg dithering | Error-diffusion to reduce banding                                                     |
| ANSI truecolor            | `\033[38;2;R;G;Bm` per-cell coloring                                                  |
| Hot-reload plugin system  | `inotify` + `dlopen` - rebuild a filter `.so`, it reloads live                        |
| FPS-capped render loop    | `CLOCK_MONOTONIC` + `nanosleep` frame pacing                                          |
| Producer/consumer threads | Double-buffered capture + render (stubbed in main loop, active in `thread_sharing.c`) |

---

## Build

```bash
git clone https://github.com/Harshit-Dhanwalkar/AsciiCam.git
cd AsciiCam/C/
make
```

Requires: `gcc`, `linux/videodev2.h` (kernel headers), `libdl`, `libpthread`.
No other external dependencies.

```
build/webcam_ascii --help
```

---

## Run

```bash
# Basic (grayscale, 80×40, /dev/video0)
./build/webcam_ascii

# Truecolor output
./build/webcam_ascii -C

# With all three plugins
./build/webcam_ascii -p build/invert.so -p build/threshold.so -p build/edge_detect.so

# Edge detection mode, custom resolution
./build/webcam_ascii -e -w 320 -h 240 -W 120 -H 50

# Dithering + inverted charset
./build/webcam_ascii -D -i
```

---

## Plugin system

Plugins are shared objects (`.so`).

```bash
gcc -O2 -fPIC -shared -Iinclude filters/my_filter.c -o build/my_filter.so
./build/webcam_ascii -p build/my_filter.so
```

**Hot-reload:** the binary watches the `.so` with `inotify`. Recompile it while the viewer is running and it reloads automatically within $\approx$100 ms.

**Runtime controls:**
| Key | Action |
|---|---|
| `↑` / `↓` | select plugin |
| `[` / `]` | param $\pm$1 |
| `{` / `}` | param $\pm$10 |
| `r` | reset param to 128 |
| `q` | quit |

---

## TODO

- [x] Adjustable capture resolution
- [x] Producer/consumer thread split (double-buffered)
- [x] Brightness / contrast adjustment
- [x] Invert brightness to charset mapping
- [x] ANSI truecolor output
- [x] Floyd-Steinberg dithering
- [x] Sobel edge detection
- [x] SIMD YUYV to grayscale (SSE2)
- [x] Hot-reload plugin system
- [x] nolibc - zero libc calls
- [ ] Custom charset via config file
- [ ] Record to `.mp4` / `.gif`
- [ ] Inter-frame delta compression
- [ ] LUT cache optimization
- [ ] Replace `pthread` with raw `futex` syscalls
- [ ] Replace `dlopen` with a minimal ELF loader

## Fixes
- [ ] [Issue #2](https://github.com/Harshit-Dhanwalkar/AsciiCam/issues/2) MacOS support
  - Rewrite `capture.c` for MacOS port using [AVFoundation](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/Articles/04_MediaCapture.html). ([Stackoverflow : how do I set up a video input using the AVFoundation framework](https://stackoverflow.com/questions/32053460/how-do-i-set-up-a-video-input-using-the-avfoundation-framework))

---

Project is under [PolyForm Noncommercial License BY-NC](LICENCE).
For commercial use contact *harshitpd1729@gmail.com*.

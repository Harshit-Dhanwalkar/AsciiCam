# AsciiCam

Real-time ASCII video from your webcam in the terminal - pure C99, no heavy runtime dependencies.

<img src="assets/demo.gif" width="325">

### Edge detection + threshold plugin

<img src="assets/demo-edgedetection.gif" width="325">

---

## Features

| Feature                       | Details                                                                                                                            |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| **YUYV to grayscale**         | SSE2 SIMD (`yuyv_to_gray_simd`) - 16 pixels per iteration on x86_64, NEON on ARM64                                                 |
| **YUYV to RGB**               | Fixed‑point BT.601 conversion for truecolor output                                                                                 |
| **Multiple render modes**     | Braille (`⠿`), Blocks (`█ ░`), ASCII ramp, Half‑block, Dots - switch live with `m`/`M`                                             |
| **Edge detection**            | Sobel, Sobel with direction, Laplacian - toggle with `x`/`X`                                                                       |
| **Hot‑reloadable charsets**   | Load `.txt` ramps from a directory, switch with `n`/`N`, monitor with inotify                                                      |
| **Pseudo‑3D depth pop**       | Parallax effect based on brightness - adjust with `+`/`-`/`v`                                                                      |
| **Floyd‑Steinberg dither**    | Error‑diffusion to reduce banding                                                                                                  |
| **ANSI truecolor**            | `\033[38;2;R;G;Bm` per‑cell coloring                                                                                               |
| **Hot‑reload plugin system**  | `inotify` + `dlopen` - rebuild a filter `.so`, it reloads live                                                                     |
| **FPS‑capped render loop**    | `CLOCK_MONOTONIC` + `nanosleep` frame pacing                                                                                       |
| **Producer/consumer threads** | Double‑buffered capture + render (stubbed in main loop, active in `thread_sharing.c`)                                              |
| **Hardware camera controls**  | V4L2 exposure, contrast, white-balance via `ioctl` — live keys `e`/`E`, `c`/`C`, `w`/`W` (Linux only; macOS/Windows display `n/a`) |
| **Cross‑platform**            | Linux (V4L2, nolibc), macOS (AVFoundation, system libc), Windows (Media Foundation)                                                |

Linux: requires `gcc`, `linux/videodev2.h` (kernel headers), `libdl`, `libpthread`.
macOS: requires `Clang` and `AVFoundation` frameworks (linked automatically).
Windows: requires MinGW-w64; links `mfplat`, `mf`, `mfreadwrite`, `mfuuid`, `ole32` (Media Foundation). See Makefile `windows` branch.

No other external dependencies.

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
| `m` / `M` | cycle render mode forward / backward |
| `x` / `X` | cycle edge detection mode forward / backward |
| `n` / `N` | cycle loaded charset forward / backward |
| `p` / `o` | increase / decrease depth-pop strength |
| `e` / `E` | hw exposure down / up _(V4L2, Linux only)_ |
| `w` / `W` | hw white-balance down / up _(V4L2, Linux only)_ |
| `c` / `C` | hw contrast down / up _(V4L2, Linux only)_ |
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
- [x] Custom charset via config file
- [x] Hardware camera controls (V4L2 exposure / contrast / white-balance)
- [x] MacOS support
- [x] Windows support (Media Foundation capture backend)
  - [ ] Windows console raw-mode and signal handling (`SetConsoleMode` / `SetConsoleCtrlHandler`)
  - [ ] Hardware controls via `IAMCameraControl` / `IAMVideoProcAmp` (capture works, controls stubbed)
  - [ ] macOS hardware controls via `AVCaptureDevice` exposure/white-balance APIs (capture works, controls stubbed)
- [ ] Cature frame resizing
- [ ] Record to `.mp4` / `.gif`
- [ ] Inter-frame delta compression
- [ ] LUT cache optimization
- [ ] Replace `pthread` with raw `futex` syscalls
- [ ] Replace `dlopen` with a minimal ELF loader
- [ ] Custom threading library using `clone()` + `futex` to eliminate `-lpthread` dependency
- [ ] Implement an ELF loader (or statically link plugins) to eliminate `-ldl` dependency
  - [ ] Color support for MacOS

## Fixes

- [x] [Issue #2](https://github.com/Harshit-Dhanwalkar/AsciiCam/issues/2) MacOS support
  - [x] Rewrite `capture.c` for MacOS port using [AVFoundation](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/Articles/04_MediaCapture.html).
- [x] `nl_calloc`: zero-fill loop was commented out (returned uninitialized memory) and the `SIZE_MAX` overflow guard ran _after_ the allocation, leaking the spurious block on overflow. Both fixed.
- [x] `nl_free`: backward coalescing was a `TODO` stub — non-LIFO frees (e.g. edge-detection scratch buffers) left permanently unmerged holes. Implemented by walking from arena start to find the preceding block.
- [x] `main.c` double-buffer bug: a second `out_buf` was `malloc`/`free`'d every frame inside the loop, shadowing the persistent pre-loop allocation. Frame-loop malloc removed; the single persistent buffer is reused for the program's lifetime.
- [x] `ascii.c` `RENDER_HALF_BLOCK` height rounding: `safe_dst_h` was rounded down to a multiple of 4 unconditionally, but half-block mode only needs multiples of 2 (it stacks 2 subpixel rows per glyph, not 4). Fixed to be mode-aware.

---

Project is under [PolyForm Noncommercial License BY-NC](LICENCE).
For commercial use contact *harshitpd1729@gmail.com*.

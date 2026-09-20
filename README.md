# AsciiCam

Real-time webcam video rendered as ASCII art directly in the terminal

Written in C99 with minimal runtime dependencies and platform-specific capture backends for Linux, macOS, and Windows.

<p align="center">
  <img src="assets/demo.gif" width="325">
  <img src="assets/demo-edgedetection.gif" width="325">
</p>

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
| **Hardware camera controls**  | V4L2 exposure, contrast, white-balance via `ioctl` - live keys `e`/`E`, `c`/`C`, `w`/`W` (Linux only; macOS/Windows display `n/a`) |
| **Cross‑platform**            | Linux (V4L2, nolibc), macOS (AVFoundation, system libc), Windows (Media Foundation)                                                |

- Linux: requires `gcc`, `linux/videodev2.h` (kernel headers), `libdl`, `libpthread`.
- macOS: requires `Clang` and `AVFoundation` frameworks (linked automatically).
- Windows: requires `MinGW-w64`; links `mfplat`, `mf`, `mfreadwrite`, `mfuuid`, `ole32` (Media Foundation).

## Architecture

```marmaid
flowchart TD
    APP["Common Application"]

    subgraph LINUX["Linux"]
        NOLIBC["__LINUX_NOLIBC__"]
        SYSCALL["Raw syscalls"]
        V4L2["V4L2"]
        INOTIFY["inotify"]

        NOLIBC --> SYSCALL
        NOLIBC --> V4L2
        NOLIBC --> INOTIFY
    end

    subgraph MAC["macOS"]
        MACLIBC["System libc"]
        AVF["AVFoundation"]
        DLOPEN["dlopen"]

        MACLIBC --> AVF
        MACLIBC --> DLOPEN
    end

    subgraph WIN["Windows"]
        CRT["MinGW CRT"]
        WIN32["Win32"]
        MF["Media Foundation"]
        LOADLIB["LoadLibrary"]

        CRT --> WIN32
        WIN32 --> MF
        WIN32 --> LOADLIB
    end

    APP --> LINUX
    APP --> MAC
    APP --> WIN
```

---

## Build

### Linux / macOS

```bash
git clone https://github.com/Harshit-Dhanwalkar/AsciiCam.git
cd AsciiCam/C/
make
```

This produces `build/webcam_ascii` and compiles any plugins found in `filters/` into `build/*.so`.

---

## Run

Run is via `make run`, which builds first and then launches the binary.
Pass arguments through the `ARGS` variable:

```bash
# Basic (grayscale, 80×40, /dev/video0)
make run

# Truecolor output
make run ARGS="-C"

# Braille rendering at 30 fps
make run ARGS="-C -m braille -f 30"

# With all three plugins
make run ARGS="-p build/invert.so -p build/threshold.so -p build/edge_detect.so"

# Edge detection mode, custom resolution
make run ARGS="-E sobel -w 320 -h 240 -W 120 -H 50"

# Dithering + inverted charset
make run ARGS="-D -i"
```

You can also run the binary directly:

```bash
./build/webcam_ascii -C -m braille
./build/webcam_ascii -p build/invert.so -p build/edge_detect.so
./build/webcam_ascii --help
```

---

## Plugin system

Plugins are dynamically loaded shared libraries. Linux currently uses `.so` plugins.

```bash
gcc -O2 -fPIC -shared -Iinclude filters/my_filter.c -o build/my_filter.so
./build/webcam_ascii -p build/my_filter.so
```

**Hot-reload:** Linux: the binary watches the `.so` with `inotify`. Recompile the plugin while the viewer is running and it reloads automatically within $\approx$100 ms.

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
| `[` / `]` | param &plusmn; 1 |
| `{` / `}` | param &plusmn; 10 |
| `r` | reset param to 128 |
| `q` | quit |

---

## TODO

- [ ] Reduce remaining libc/runtime dependencies
  - [ ] Eliminate `-lc`
  - [ ] Replace `pthread` with raw `futex`/`clone`
  - [ ] Replace `dlopen` with a minimal ELF loader
- [x] Adjustable capture resolution
- [x] Producer/consumer thread split : Double-buffered capture/render architecture; implementation in `thread_sharing.c`
- [x] Brightness / contrast adjustment
- [x] Invert brightness to charset mapping
- [x] ANSI truecolor output
- [x] Floyd-Steinberg dithering
- [x] Sobel edge detection
- [x] SIMD YUYV to grayscale (SSE2)
- [x] Hot-reload plugin system
- [x] Custom charset via config file
- [x] Hardware camera controls (V4L2 exposure / contrast / white-balance)
- [x] macOS support
  - [ ] Color support for macOS
- [ ] Windows support (Media Foundation capture backend)
  - [ ] Windows console raw-mode and signal handling (`SetConsoleMode` / `SetConsoleCtrlHandler`)
  - [ ] Hardware controls via `IAMCameraControl` / `IAMVideoProcAmp` (capture works, controls stubbed)
  - [ ] macOS hardware controls via `AVCaptureDevice` exposure/white-balance APIs (capture works, controls stubbed)
- [ ] Capture frame resizing
  - [x] Auto frame resizing (depends on terminal `w` and `h`)
  - [ ] Frame resizing using cursor
- [ ] Record to `.mp4` / `.gif`
- [ ] Inter-frame delta compression
- [ ] LUT cache optimization
- [ ] Implement an ELF loader (or statically link plugins) to eliminate `-ldl` dependency

## Fixes

- [x] [Issue #2](https://github.com/Harshit-Dhanwalkar/AsciiCam/issues/2) macOS support
  - [x] Rewrite `capture.c` for macOS port using [AVFoundation](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/Articles/04_MediaCapture.html).

---

Project is under [PolyForm Noncommercial License BY-NC](LICENCE).
For commercial use contact *harshitpd1729@gmail.com*.

---

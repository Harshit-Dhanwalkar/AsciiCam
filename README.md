# AsciiCam

[![CI](https://github.com/Harshit-Dhanwalkar/AsciiCam/actions/workflows/ci.yml/badge.svg)](https://github.com/Harshit-Dhanwalkar/AsciiCam/actions/workflows/ci.yml)
[![License: PolyForm NC](https://img.shields.io/badge/License-PolyForm_Noncommercial-red.svg)](https://polyformproject.org/licenses/noncommercial/1.0.0/)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-blue)
![Language](https://img.shields.io/badge/language-C99-orange)

Real-time webcam video rendered as ASCII art directly in the terminal

Written in C99. Ships its own minimal libc layer (`nolibc`) on Linux: the binary is
statically linked with no runtime dependencies (plugins load through its own
ELF loader, `nl_dlfcn`). Uses platform-native capture backends:
V4L2 on Linux, AVFoundation on macOS, Media Foundation on Windows.

<p align="center">
  <img src="assets/demo.gif" width="325">
  <img src="assets/demo-edgedetection.gif" width="325">
</p>

---

## Features

| Feature                      | Details                                                                                                                            |
| ---------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| **YUYV to grayscale**        | SSE2 SIMD (`yuyv_to_gray_simd`) - 16 pixels per iteration on x86_64, NEON on ARM64                                                 |
| **YUYV to RGB**              | Fixed‑point BT.601 conversion for truecolor output                                                                                 |
| **Render modes**             | Braille, Blocks, ASCII ramp, Half-block, Dots - cycle live with `m`/`M`                                                            |
| **Edge detection**           | Sobel, Sobel with direction, Laplacian - toggle with `x`/`X`                                                                       |
| **Image adjustments**        | Brightness, contrast, gamma (`-g 10..400`), invert mapping                                                                         |
| **Pseudo-3D depth pop**      | Parallax displacement based on brightness — `+`/`-`/`v`                                                                            |
| **Floyd‑Steinberg dither**   | Error‑diffusion to reduce banding                                                                                                  |
| **ANSI color**               | Truecolor (`-C`) or xterm-256 (`-2`) per-cell foreground                                                                           |
| **ANSI truecolor**           | `\033[38;2;R;G;Bm` per‑cell coloring                                                                                               |
| **Hot‑reloadable charsets**  | Load `.txt` ramps from a directory, switch with `n`/`N`, monitor with inotify                                                      |
| **Hot‑reload plugin system** | `inotify` + `dlopen` - rebuild a filter `.so`, it reloads live                                                                     |
| **SIGWINCH auto-resize**     | Terminal resize re-queries `TIOCGWINSZ` and reallocates buffers                                                                    |
| **FPS‑capped render loop**   | `CLOCK_MONOTONIC` + `nanosleep` frame pacing                                                                                       |
| **Capture thread**           | Linux: capture + luma conversion on a `clone()`d thread, triple-buffered hand-off to the render loop; `-T` captures inline         |
| **Hardware camera controls** | V4L2 exposure, contrast, white-balance via `ioctl` - live keys `e`/`E`, `c`/`C`, `w`/`W` (Linux only; macOS/Windows display `n/a`) |
| **Cross-platform**           | Linux (V4L2, nolibc), macOS (AVFoundation, system libc), Windows (Media Foundation, MinGW)                                         |

- Linux: requires `gcc` only (no kernel headers, `libdl` or `libpthread`).
- macOS: requires `Clang` and `AVFoundation` frameworks (linked automatically).
- Windows: requires `MinGW-w64`; links `mfplat`, `mf`, `mfreadwrite`, `mfuuid`, `ole32` (Media Foundation).

## Architecture

```mermaid
flowchart TD
    APP["Application\n(main.c, ascii.c)"]

    NOLIBC["nolibc.h\n(umbrella header)"]
    THREADS["nl_thread.h\nfutex + clone"]
    IO["nl_io.h\nsyscall wrappers"]
    DLFCN["nl_dlfcn.h\nLoadLibrary / dlopen"]

    subgraph LINUX["Linux (__LINUX_NOLIBC__)"]
        SYSCALL["Raw syscalls"]
        V4L2["V4L2 / ioctl"]
        INOTIFY["inotify"]
    end

    subgraph MAC["macOS"]
        MACLIBC["System libc"]
        AVF["AVFoundation"]
    end

    subgraph WIN["Windows"]
        CRT["MinGW CRT"]
        MF["Media Foundation"]
    end

    APP --> NOLIBC
    NOLIBC --> THREADS
    NOLIBC --> IO
    NOLIBC --> DLFCN

    IO --> SYSCALL
    IO --> V4L2
    IO --> INOTIFY
    IO -. "system libc" .-> MACLIBC
    IO -. "Win32 API" .-> CRT

    MACLIBC --> AVF
    CRT --> MF

    DLFCN --> LINUX
    DLFCN --> WIN
```

On Linux, the application makes raw `syscall(2)` invocations instead of
calling into glibc. Threading (when enabled) uses `clone()` + `futex(2)`
directly - no `libpthread`. Plugins are loaded by a from-scratch ELF loader
(`nl_dlfcn`), so the Linux binary is fully static with no glibc dependency.
macOS and Windows still use their system libraries.

---

## Build

```bash
git clone https://github.com/Harshit-Dhanwalkar/AsciiCam.git
```

or via ssh

```bash
git clone git@github.com:Harshit-Dhanwalkar/AsciiCam.git
```

### Linux / macOS

Navigate in `C` directory and build

```bash
cd AsciiCam/C/
make
```

### Windows (MinGW-w64)

From a Git Bash prompt with MinGW-w64 on `PATH`:

```bash
cd AsciiCam/C
make CC=gcc
```

This produces build/webcam_ascii (.exe on Windows) and compiles any
plugins in filters/ into build/\*.{so,dylib,dll}.

### Requirements:

- Linux: `gcc` (V4L2 ABI and everything else is vendored under `lib/`)
- macOS: `clang`, AVFoundation (linked automatically)
- Windows: MinGW-w64; links `mfplat`, `mf`, `mfreadwrite`, `mfuuid`, `ole32`

### Tests (Linux)

`make test` builds and runs the dependency-free unit tests (allocator, printf,
ELF loader, fuzz, mouse, capture reinit, plugin catalog, threads, capture
pipeline, filter security)

`make e2e` runs the real main loop against a software camera in a pty (threaded
and `-T`, colour, padded rows, and a wedged camera) - no webcam needed.

## Run

Run is via `make run`, which builds first and then launches the binary.
Pass arguments through the `ARGS` variable:

```bash
# Basic (grayscale, 80x40, /dev/video0)
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

# Gamma correction: 50 = darker midtones, 200 = brighter
make run ARGS="-g 200"

# 256-color output (for terminals without truecolor)
make run ARGS="-2"
```

You can also run the binary directly:

```bash
./build/webcam_ascii -C -m braille
./build/webcam_ascii -p build/invert.so -p build/edge_detect.so
./build/webcam_ascii --help
```

### Config file:

AsciiCam reads ./.asciicamrc on startup if present. Keys mirror the CLI
flags and are overridden by any flag passed on the command line.

```bash
# .asciicamrc
device = /dev/video0
capture_width = 640
capture_height = 480
fps = 30
render_mode = braille
color = 1
color_mode = 256
gamma = 150
plugin = build/invert.so
```

---

## Plugin system

Plugins are shared libraries (`.so` on Linux, `.dylib` on macOS, `.dll` on
Windows). Hot-reload is only available on Linux; the macOS and Windows
builds load plugins once at startup.

```bash
gcc -O2 -fPIC -shared -Iinclude filters/my_filter.c -o build/my_filter.so
./build/webcam_ascii -p build/my_filter.so
```

**Hot-reload:** Linux: the binary watches the `.so` with `inotify`. Recompile the plugin while the viewer is running and it reloads automatically within $\approx$100 ms.

### Shipping plugins

The program binary contains **no plugin code** (static binary ≈ 86 KB, each
filter ≈ 15 KB), so plugins are separate files and have to be shipped with it.
Which files depends on what you want the user to be able to do:

| You ship                   | User can                                         | Needs on the target   |
| -------------------------- | ------------------------------------------------ | --------------------- |
| binary + `plugins/*.so`    | load every shipped plugin instantly with `/`     | nothing (no compiler) |
| + SDK (`make install-sdk`) | also compile new plugins from `.c` while running | `make` + `gcc`        |

Lookup is relative to the running binary, not the current directory, so the same
build works from a checkout, from an unpacked release and from an installed
prefix:

```text
checkout    build/webcam_ascii             sources filters/, cache .cache/plugins
release     asciicam  plugins/*.so         (zip these two, unpack anywhere)
installed   <prefix>/bin/asciicam          <prefix>/share/asciicam/{plugins,charsets,sdk}
```

```bash
make install PREFIX=/usr/local        # binary + prebuilt plugins + charsets
make install-sdk PREFIX=/usr/local    # ... plus Makefile, headers, plugin sources
```

Prebuilt libraries are preferred; a plugin is only compiled when the picker is
asked to load one that has no prebuilt library, an SDK is present and `make`
and a compiler are found. Runtime builds go to `.cache/plugins` in a checkout,
otherwise to `$XDG_CACHE_HOME/asciicam/plugins` (`~/.cache/asciicam/plugins`),
so a read-only install prefix is fine. A plugin built from other headers is
refused with a message instead of crashing: put `PLUGIN_ABI_EXPORT;` next to
`plugin_get()` in your plugin so it carries the interface version
(`PLUGIN_ABI` in `include/plugins.h`; plugins without it still load).
`-L` / `plugin_dir`, `plugin_cache` and `-k` / `charset_dir` override the
automatic locations.

### Mouse

| Mouse                                  | Action                                                      |
| -------------------------------------- | ----------------------------------------------------------- |
| drag any of the four corners (◤ ◥ ◣ ◢) | resize the frame; the opposite corner stays where it is     |
| drag inside the frame                  | move it anywhere in the terminal                            |
| `a`                                    | back to automatic fit (top-left, follows the terminal size) |

An outline shows where the frame will end up and nothing is changed until you
release the button. The status panel stays at the bottom of the terminal.

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
| `g` / `G` | decrease / increase gamma |
| `↑` / `↓` | select plugin |
| `[` / `]` | param &plusmn; 1 |
| `{` / `}` | param &plusmn; 10 |
| `r` | reset param to 128 |
| `q` | quit |

---

## TODO

- [x] Reduce remaining libc/runtime dependencies
  - [x] Eliminate `-lc`
  - [x] Replace `pthread` with raw `futex`/`clone`
    - [x] `nl_thread.h`: futex + `clone` implementation
    - [x] Triple-buffered capture thread active in main loop (Linux; `-T` for inline)
  - [x] Replace `dlopen` with a minimal ELF loader
  - [x] Implement an ELF loader (or statically link plugins) to eliminate `-ldl` dependency
- [x] Adjustable capture resolution
- [x] Brightness / contrast / gamma adjustment
- [x] Invert brightness to charset mapping
- [x] ANSI truecolor and 256-color output
- [x] Floyd-Steinberg dithering
- [x] Sobel / Sobel-direction / Laplacian edge detection
- [x] SIMD YUYV to grayscale (SSE2)
- [x] Hot-reload plugin system
- [x] Relocatable plugin lookup, prebuilt-first loading, `make install` / `install-sdk`
- [x] Config file (`./.asciicamrc`)
- [x] Capture frame resizing
  - [x] Auto resize on terminal `SIGWINCH`
  - [x] Manual resize with cursor drag (all four corners)
  - [x] Move the frame anywhere in the terminal by dragging it
- [x] Custom charset via config file
- [x] Hardware camera controls (Linux V4L2)
- [x] macOS support
  - [ ] Color support for macOS
- [ ] Windows support (Media Foundation capture backend)
  - [x] Windows capture via Media Foundation
  - [ ] Windows console raw-mode and signal handling (`SetConsoleMode` / `SetConsoleCtrlHandler`)
  - [ ] Hardware controls via `IAMCameraControl` / `IAMVideoProcAmp` (capture works, controls stubbed)
  - [ ] macOS hardware controls via `AVCaptureDevice` exposure/white-balance APIs (capture works, controls stubbed)
- [x] Runtime plugin catalog: search, build and load plugins while running (`/`)

## Fixes

- [x] [Issue #2](https://github.com/Harshit-Dhanwalkar/AsciiCam/issues/2) macOS support
  - [x] Rewrite `capture.c` for macOS port using [AVFoundation](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/Articles/04_MediaCapture.html).

---

Project is under [PolyForm Noncommercial License BY-NC](LICENCE).
For commercial use contact *harshitpd1729@gmail.com*.

---

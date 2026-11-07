#!/usr/bin/env python3
"""End-to-end check of real main loop against software camera

Usage: e2e_capture.py <asciicam-fake-binary>

Runs binary in a pty (it needs a terminal for raw mode), once with
capture thread and once with -T, and checks:
  * frames keep being drawn at about requested rate
  * colour output works, including with padded camera rows
  * 'q' quits cleanly (exit 0, cursor restored)
  * with a wedged camera, 'q' is still handled promptly when a capture thread
    is used (inline capture blocks in camera wait)
"""
import fcntl
import json
import os
import pty
import re
import select
import shutil
import struct
import sys
import tempfile
import termios
import time

REAL_HOME = os.environ.get("HOME", "")
START_DIR = os.getcwd()

BIN = sys.argv[1]
if not os.access(BIN, os.X_OK):
    sys.exit(f"e2e: {BIN} is not an executable (build it with `make e2e`)")
SYNC_BEGIN = b"\x1b[?2026h"
failures = 0


def check(cond, msg):
    global failures
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        failures += 1


def run(args, seconds, quit_after=None, key=b"q"):
    """Returns (frames, exit_code, output, quit_latency_seconds)."""
    pid, fd = pty.fork()
    if pid == 0:
       # size terminal before program starts: setting it from  parent
       # races with startup and can trigger a SIGWINCH auto-fit
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
        os.execv(BIN, [BIN] + args)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))

    out = b""
    start = time.monotonic()
    sent_at = None
    deadline = start + seconds + 6
    status = None
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if not data:
                break
            out += data
        now = time.monotonic()
        if sent_at is None and now - start >= (quit_after if quit_after is not None else seconds):
            os.write(fd, key)
            sent_at = now
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
            break
    if status is None:
        # pty EOF arrives as child exits, often before waitpid sees it
        for _ in range(100):
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
                break
            time.sleep(0.02)
        else:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
    latency = (time.monotonic() - sent_at) if sent_at else None
    code = os.waitstatus_to_exitcode(status) if status is not None else -1
    return out.count(SYNC_BEGIN), code, out, latency


for label, extra in (("threaded", []), ("inline -T", ["-T"])):
    frames, code, out, _ = run(["-d", "fake", "-r", "20"] + extra, 2.0)
    check(frames >= 25, f"{label}: frames drawn in 2 s ({frames})")
    check(code == 0, f"{label}: 'q' exits 0 (got {code})")
    check(b"\x1b[?25h" in out, f"{label}: cursor restored on exit")

    frames, code, out, _ = run(["-d", "fake:pad", "-r", "20", "-C", "-w", "320", "-h", "240"] + extra, 1.5)
    check(frames >= 15 and code == 0, f"{label}: padded rows + colour ({frames} frames, exit {code})")
    check(b"\x1b[38;2;" in out, f"{label}: truecolor escapes emitted")

# Wedged camera: capture thread keeps UI responsive, inline capture does not
lat = {}
for label, extra in (("threaded", []), ("inline -T", ["-T"])):
    frames, code, out, latency = run(["-d", "fake:stall=10", "-r", "20"] + extra, 3.0, quit_after=1.5)
    lat[label] = latency
    check(code == 0, f"{label}: stalled camera, 'q' still exits 0 (got {code})")
    if latency is not None:
        print(f"     {label}: quit latency with a stalled camera = {latency:.2f}s")

check(lat["threaded"] is not None and lat["threaded"] < 0.6,
      f"threaded: quits promptly with a stalled camera ({lat['threaded']})")


# Mouse: move and resize frame in a live session
def session(args, steps, settle=0.6):
    """steps: list of (bytes_to_send, seconds_to_wait). Returns all output."""
    pid, fd = pty.fork()
    if pid == 0:
        # size terminal before program starts: setting it from   parent races
        # with startup and can trigger a SIGWINCH auto-fit
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
        os.execv(BIN, [BIN] + args)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
    out = b""
    marks = []

    def pump(t):
        nonlocal out
        end = time.monotonic() + t
        while time.monotonic() < end:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                out += data

    pump(settle)
    marks.append(len(out))
    for data, wait in steps + [(b"q", 0.4)]:
        os.write(fd, data)
        pump(wait)
        marks.append(len(out))
    for _ in range(100):
        done, _ = os.waitpid(pid, os.WNOHANG)
        if done:
            break
        time.sleep(0.02)
    else:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    return out, marks


def mouse(b, x, y, release=False):
    return b"\x1b[<%d;%d;%d%s" % (b, x, y, b"m" if release else b"M")


def last_frame(buf):
    m = re.findall(rb"frame: (\d+)x(\d+)", buf)
    return tuple(map(int, m[-1])) if m else None


def last_origin(buf):
    # frames are drawn starting with a cursor move to their top-left cell
    m = re.findall(rb"\x1b\[\?2026h\x1b\[(?:(\d+);(\d+))?H", buf)
    if not m:
        return None
    y, x = m[-1]
    return (int(x), int(y)) if x else (1, 1)


ARGS = ["-d", "fake", "-W", "40", "-H", "20", "-r", "20"]
for label, extra in (("threaded", []), ("inline -T", ["-T"])):
    out, marks = session(
        ARGS + extra,
        [
            (mouse(0, 10, 5), 0.15),            # grab frame body
            (mouse(32, 30, 15), 0.3),           # drag +20,+10
            (mouse(0, 30, 15, True), 0.5),      # release
        ],
    )
    during = out[marks[1]:marks[3]]
    check(b"MOVING frame (1,1) -> (21,11)" in during, f"{label}: move shows pending position")
    check(b"\x1b[11;21H+-" in during,
          f"{label}: outline drawn at pending position")
    after = out[marks[-3]:]
    check(last_origin(after) == (21, 11), f"{label}: frame redrawn at (21,11), got {last_origin(after)}")
    check(last_frame(after) == (40, 20), f"{label}: size unchanged by a move ({last_frame(after)})")

    # then resize from bottom-right corner (60,30) and top-left (21,11)
    out, marks = session(
        ARGS + extra,
        [
            (mouse(0, 10, 5), 0.15), (mouse(32, 30, 15), 0.2), (mouse(0, 30, 15, True), 0.4),
            (mouse(0, 60, 30), 0.15), (mouse(32, 70, 35), 0.25), (mouse(0, 70, 35, True), 0.5),
        ],
    )
    after = out[marks[-3]:]
    check(last_frame(after) == (50, 25), f"{label}: BR corner resize -> 50x25 ({last_frame(after)})")
    check(last_origin(after) == (21, 11), f"{label}: BR resize keeps top-left ({last_origin(after)})")

    out, marks = session(
        ARGS + extra,
        [
            (mouse(0, 10, 5), 0.15), (mouse(32, 30, 15), 0.2), (mouse(0, 30, 15, True), 0.4),
            (mouse(0, 21, 11), 0.15), (mouse(32, 16, 8), 0.25), (mouse(0, 16, 8, True), 0.5),
        ],
    )
    after = out[marks[-3]:]
    check(last_frame(after) == (45, 23), f"{label}: TL corner resize -> 45x23 ({last_frame(after)})")
    check(last_origin(after) == (16, 8), f"{label}: TL resize keeps bottom-right fixed ({last_origin(after)})")

    # colour at full camera size: RGB copy and colour output buffer used to
    # fill allocator arena, so every resize silently failed
    out, marks = session(
        ["-d", "fake", "-w", "640", "-h", "480", "-C", "-W", "100", "-H", "40",
         "-r", "20"] + extra,
        [
            (mouse(0, 100, 40), 0.15), (mouse(32, 90, 35), 0.25), (mouse(0, 90, 35, True), 0.5),
        ],
    )
    after = out[marks[-3]:]
    check(last_frame(after) == (90, 35), f"{label}: colour 640x480 BR resize -> 90x35 ({last_frame(after)})")

    # a click without dragging changes nothing; 'a' goes back to top-left
    out, marks = session(
        ARGS + extra,
        [
            (mouse(0, 10, 5), 0.15), (mouse(0, 10, 5, True), 0.3),
            (mouse(0, 10, 5), 0.15), (mouse(32, 30, 15), 0.2), (mouse(0, 30, 15, True), 0.4),
            (b"a", 0.6),
        ],
    )
    check(last_origin(out[marks[0]:marks[2]]) == (1, 1), f"{label}: plain click does not move frame")
    check(last_origin(out[marks[-3]:marks[-2]]) == (1, 1), f"{label}: 'a' returns frame to top-left")
    check(last_frame(out[marks[-3]:marks[-2]]) == (120, 46), f"{label}: 'a' fits terminal ({last_frame(out[marks[-3]:marks[-2]])})")

    # a terminal may deliver one mouse report in pieces (slow link, tmux, ssh):
    # same drag byte by byte must resize just same
    def trickle(data):
        return [(bytes([c]), 0.004) for c in data]

    steps = trickle(mouse(0, 40, 20))
    steps += [(b"", 0.15)] + trickle(mouse(32, 30, 15))
    steps += [(b"", 0.2)] + trickle(mouse(0, 30, 15, True)) + [(b"", 0.5)]
    out, marks = session(ARGS + extra, steps)
    check(last_frame(out) == (30, 15),
          f"{label}: drag delivered byte by byte still resizes ({last_frame(out)})")
    check(out.count(b"frame: ") >= 1 and b"frame: 30x15" in out,
          f"{label}: no hotkeys triggered by a split report")

# Recorder + screenshot: keys, red REC clock, toasts, files that really decode
def check_png(path):
    import zlib
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w = 8, b"", None
    while pos < len(d):
        n = struct.unpack(">I", d[pos:pos + 4])[0]
        typ, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        if zlib.crc32(typ + body) != struct.unpack(">I", d[pos + 8 + n:pos + 12 + n])[0]:
            return None
        if typ == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", body[:10])
        if typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    ch = 3 if ct == 2 else 1
    return (w, h, ch) if len(raw) == h * (1 + w * ch) else None

# options cut out of argv (--net) must not hide environment
cap_root = tempfile.mkdtemp(prefix="asciicam_net_")
os.environ["HOME"] = cap_root
out, marks = session(ARGS + ["--net", "100"], [(b"s", 0.8)])
os.environ["HOME"] = REAL_HOME
net_dir = os.path.join(cap_root, "Pictures", "asciicam")
check(os.path.isdir(net_dir) and any(f.endswith(".png") for f in os.listdir(net_dir)),
      "--net keeps HOME: screenshot saved under ~/Pictures/asciicam")

for label, extra in (("colour", ["-C"]), ("mono", [])):
    # captures go to ~/Pictures/asciicam: point HOME at a scratch directory so 
    # a test run never writes into real one
    cap_root = tempfile.mkdtemp(prefix="asciicam_cap_")
    os.environ["HOME"] = cap_root
    out, marks = session(
        ARGS + extra,
        [
            (b"t", 0.9),     # start recording
            (b"s", 0.5),     # screenshot while recording
            (b"t", 0.5),     # stop
        ],
    )
    os.environ["HOME"] = REAL_HOME
    cap_dir = os.path.join(cap_root, "Pictures", "asciicam")
    files = sorted(os.listdir(cap_dir)) if os.path.isdir(cap_dir) else []
    casts = [f for f in files if f.endswith(".cast")]
    pngs = [f for f in files if f.endswith(".png")]
    ansis = [f for f in files if f.endswith(".ansi")]
    check(len(casts) == 1 and len(pngs) == 1 and len(ansis) == 1,
          f"recorder {label}: one .cast, .png, .ansi saved ({files})")
    check(b"REC started" in out, f"recorder {label}: toast on start")
    check(b"REC stopped" in out and b"saved" in out,
          f"recorder {label}: toasts on stop and screenshot")
    check(b"asciicam-" in out, f"recorder {label}: toast names the saved file")
    check(b" REC 0:0" in out, f"recorder {label}: REC clock drawn in panel")
    if label == "colour":
        check(b"\x1b[1;38;2;255;40;40m\xe2\x97\x8f" in out,
              "recorder colour: red dot")
    else:
        check(b"(o) REC" in out, "recorder mono: (o) dot")
    if casts:
        lines = open(os.path.join(cap_dir, casts[0]), encoding="utf-8").read().split("\n")
        head = json.loads(lines[0])
        events = [json.loads(l) for l in lines[1:] if l]
        check(head["version"] == 2 and head["width"] == 120 and head["height"] == 50,
              f"recorder {label}: cast header {head}")
        check(len(events) >= 8 and all(e[1] == "o" for e in events),
              f"recorder {label}: {len(events)} output events")
        times = [e[0] for e in events]
        check(times == sorted(times) and 0.4 < times[-1] < 2.0,
              f"recorder {label}: event times rise, last {times[-1] if times else None}")
        check(any("\x1b[" in e[2] for e in events), f"recorder {label}: frames hold the escapes")
        check(not any("REC" in e[2] for e in events),
              f"recorder {label}: panel/REC clock not part of recording")
    if pngs:
        info = check_png(os.path.join(cap_dir, pngs[0]))
        check(info == (640, 480, 3 if label == "colour" else 1),  # camera size, not -W/-H
              f"recorder {label}: png decodes, {info}")
    if ansis:
        txt = open(os.path.join(cap_dir, ansis[0]), "rb").read()
        check(txt.startswith(b"\x1b[2J\x1b[H") and len(txt) > 200,
              f"recorder {label}: .ansi holds the frame")
    shutil.rmtree(cap_root, ignore_errors=True)

# unwritable captures folder (and an unwritable working directory, which is last-resort
# fallback): a message, no crash
os.environ["HOME"] = "/proc/nope"
BIN_REL = BIN
BIN = os.path.abspath(BIN)
os.chdir("/proc")
try:
    out, marks = session(ARGS, [(b"t", 0.5), (b"s", 0.5)])
finally:
    os.chdir(START_DIR)
    BIN = BIN_REL
    os.environ["HOME"] = REAL_HOME
check(b"cannot record" in out and b"cannot save" in out,
      "recorder: unwritable folder gives messages, program keeps running")
check(out.count(SYNC_BEGIN) > 10, "recorder: still drawing after failures")


# Real binary with built-in picture: no camera, no fake
REAL = sys.argv[2] if len(sys.argv) > 2 else None
if REAL and os.access(REAL, os.X_OK):
    BIN_SAVE = BIN
    BIN = REAL
    for flag in (["--demo"], ["-d", "demo"]):
        frames, code, out, _ = run(flag + ["-r", "20", "-C"], 1.5)
        check(frames >= 15 and code == 0, f"real binary {' '.join(flag)}: picture drawn ({frames} frames, exit {code})")
        check(b"\x1b[38;2;" in out, f"real binary {' '.join(flag)}: colour")
    frames, code, out, latency = run(["--demo", "-r", "20"], 1.0)
    check(len(set(re.findall(rb"\x1b\[\?2026h(.{200})", out, re.DOTALL))) > 3,
          "real binary --demo: picture animates")

    # every effect plugin loads through the real loader and keeps the picture
    # moving (plugin lookups are relative to the binary: build/<name>.so)
    plugin_dir = os.path.dirname(os.path.abspath(REAL))
    for name in ['chromatic', 'crt', 'dither_bayer', 'dither_fs', 'fisheye', 'framediff', 'glitch', 'kaleidoscope', 'matrix_rain', 'neon_edge', 'pixel_sort', 'ripple', 'sepia', 'thermal', 'trails', 'tv_static', 'vhs', 'vignette']:
        so = os.path.join(plugin_dir, name + ".so")
        if not os.path.exists(so):
            check(False, f"plugin {name}: {so} was built")
            continue
        frames, code, out, _ = run(["--demo", "-r", "20", "-p", so], 1.0)
        check(frames >= 8 and code == 0 and b"Failed to load" not in out,
              f"plugin {name}: loads and draws ({frames} frames, exit {code})")

    # slow-link mode: draws and a bad value is refused up front
    for level in ("0", "300"):
        frames, code, out, _ = run(["--demo", "-f", "20", "--net", level], 1.0)
        check(frames >= 8 and code == 0,
              f"--net {level}: draws and exits cleanly ({frames} frames, exit {code})")
    _, code, out, _ = run(["--demo", "--net", "fast"], 1.0)
    check(code == 2 and b"--net needs" in out, "--net with a bad value is refused")
    _, code, out, _ = run(["--demo", "--net"], 1.0)
    check(code == 2, "--net without a value is refused")

    # --play: replay a .cast with its timing, no camera (this is what runs over ssh)
    play_dir = tempfile.mkdtemp(prefix="asciicam_play_")
    cast = os.path.join(play_dir, "t.cast")
    with open(cast, "w", encoding="utf-8") as f:
        f.write(json.dumps({"version": 2, "width": 60, "height": 20}) + "\n")
        f.writelines(json.dumps([t, "o", d]) + "\n" for t, d in ((0.0, "\x1b[2J\x1b[Hone"), (0.5, "\x1b[Htwo"), (1.0, "\x1b[Hthree \u00e9")))

    def play(args, steps=(), total=3.0):
        """Run --play in a pty; steps = [(seconds, bytes)]. Returns (code, [(t, data)])."""
        pid, fd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
            os.execv(BIN, [BIN] + args)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        t0, chunks, pending, status = time.monotonic(), [], sorted(steps), None
        while time.monotonic() - t0 < total and status is None:
            while pending and time.monotonic() - t0 >= pending[0][0]:
                os.write(fd, pending.pop(0)[1])
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    d = b""
                if d:
                    chunks.append((time.monotonic() - t0, d))
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
        if status is None:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            return None, chunks
        return os.waitstatus_to_exitcode(status), chunks

    def when(chunks, word):
        for t, d in chunks:
            if word in d:
                return t
        return None

    code, ch = play(["--play", cast])
    t_two, t_three = when(ch, b"two"), when(ch, b"three")
    check(code == 0 and t_two and t_three, f"--play: plays to the end, exit {code}")
    check(t_two and 0.4 < t_two < 0.8 and t_three and 0.9 < t_three < 1.4,
          f"--play: keeps recorded timing (0.5 s / 1.0 s, got {t_two and round(t_two, 2)} / {t_three and round(t_three, 2)})")
    check(b"\xc3\xa9" in b"".join(d for _, d in ch), "--play: UTF-8 bytes arrive intact")
    check(b"\x1b[?1049l" in ch[-1][1] and b"\x1b[?25h" in b"".join(d for _, d in ch), "--play: screen and cursor restored")

    code, ch = play(["--play", cast, "--speed", "2"])
    t_three = when(ch, b"three")
    check(code == 0 and t_three and t_three < 0.8, f"--play --speed 2: twice as fast ({t_three and round(t_three, 2)})")

    code, ch = play(["--play", cast], steps=[(0.7, b" "), (2.2, b" ")], total=4.0)
    t_three = when(ch, b"three")
    check(code == 0 and t_three and 2.3 < t_three < 3.0,
          f"--play: space pauses and resumes ({t_three and round(t_three, 2)})")

    code, ch = play(["--play", cast, "--loop"], steps=[(2.6, b"q")], total=5.0)
    check(code == 0 and sum(1 for _, d in ch if b"two" in d) >= 2, "--play --loop: repeats, q quits with exit 0")

    code, ch = play(["--play", cast], steps=[(0.2, b"q")], total=3.0)
    check(code == 0 and when(ch, b"three") is None, "--play: q stops early")

    code, ch = play(["--play", os.path.join(play_dir, "missing.cast")])
    check(code == 1 and b"cannot read" in b"".join(d for _, d in ch) and b"\x1b[?1049h" not in b"".join(d for _, d in ch),
          f"--play: missing file refused before touching the screen (exit {code})")
    code, ch = play(["--play", cast, "--speed", "fast"])
    check(code == 2 and b"--speed needs" in b"".join(d for _, d in ch), "--play: bad --speed refused")
    code, ch = play(["--play"])
    check(code == 2, "--play without a file is refused")
    shutil.rmtree(play_dir, ignore_errors=True)



    # a missing camera explains how to get a picture
    frames, code, out, _ = run(["-d", "/dev/definitely-not-a-camera"], 0.3)
    check(code != 0 and b"--demo" in out, "missing camera: error mentions --demo")
    BIN = BIN_SAVE

print("e2e:", "FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)

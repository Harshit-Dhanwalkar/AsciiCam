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
import os
import pty
import re
import select
import struct
import sys
import termios
import time

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
    frames, code, out, _ = run(["-d", "fake", "-f", "20"] + extra, 2.0)
    check(frames >= 25, f"{label}: frames drawn in 2 s ({frames})")
    check(code == 0, f"{label}: 'q' exits 0 (got {code})")
    check(b"\x1b[?25h" in out, f"{label}: cursor restored on exit")

    frames, code, out, _ = run(["-d", "fake:pad", "-f", "20", "-C", "-w", "320", "-h", "240"] + extra, 1.5)
    check(frames >= 15 and code == 0, f"{label}: padded rows + colour ({frames} frames, exit {code})")
    check(b"\x1b[38;2;" in out, f"{label}: truecolor escapes emitted")

# Wedged camera: capture thread keeps UI responsive, inline capture does not
lat = {}
for label, extra in (("threaded", []), ("inline -T", ["-T"])):
    frames, code, out, latency = run(["-d", "fake:stall=10", "-f", "20"] + extra, 3.0, quit_after=1.5)
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


ARGS = ["-d", "fake", "-W", "40", "-H", "20", "-f", "20"]
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


print("e2e:", "FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)

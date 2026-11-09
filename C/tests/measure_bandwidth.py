#!/usr/bin/env python3
"""Bytes per frame program writes to terminal, with and without --net

    python3 tests/measure_bandwidth.py build/asciicam [seconds]

Runs demo source in a pseudo-terminal for each render/colour mode and
--net setting and prints bytes per frame and kbit/s (frames are counted
by synchronized-update markers)
The panel and overlays are included, so figures are what actually crosses wire"""

import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

BIN = os.path.abspath(sys.argv[1])
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0


def run(args, secs, rows=40, cols=100):
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.execv(BIN, [BIN] + args)
    out, t0, quit_sent = b"", time.monotonic(), False
    while time.monotonic() - t0 < secs + 4:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 1 << 20)
            except OSError:
                break
            if not d:
                break
            out += d
        if not quit_sent and time.monotonic() - t0 > secs:
            os.write(fd, b"q")
            quit_sent = True
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return out


modes = [("ascii mono", ["-m", "ascii"]), ("ascii 256-colour", ["-m", "ascii", "-2"]),
         ("ascii truecolor", ["-m", "ascii", "-C"]),
         ("halfblock truecolor", ["-m", "halfblock", "-C"])]
nets = [("off", []), ("--net 0", ["--net", "0"]), ("--net 1000", ["--net", "1000"]),
        ("--net 300", ["--net", "300"]), ("--net 100", ["--net", "100"])]
print(f"{'mode':20s}{'setting':12s}{'B/frame':>9s}{'kbit/s':>9s}")
for mname, margs in modes:
    for nname, nargs in nets:
        out = run(["--demo", "-f", "20"] + margs + nargs, SECS)
        frames = max(out.count(b"\x1b[?2026h"), 1)
        print(f"{mname:20s}{nname:12s}{len(out) // frames:9d}{len(out) * 8 / SECS / 1000:9.0f}")


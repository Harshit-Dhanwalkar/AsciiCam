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
import random
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
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
        # Size terminal before program starts: setting it from parent races with
        # startup and can trigger a SIGWINCH auto-fit
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
    # Frames are drawn starting with a cursor move to their top-left cell
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

    # Then resize from bottom-right corner (60,30) and top-left (21,11)
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

    # Colour at full camera size: RGB copy and colour output buffer used to fill allocator
    # arena, so every resize silently failed
    out, marks = session(
        ["-d", "fake", "-w", "640", "-h", "480", "-C", "-W", "100", "-H", "40",
         "-r", "20"] + extra,
        [
            (mouse(0, 100, 40), 0.15), (mouse(32, 90, 35), 0.25), (mouse(0, 90, 35, True), 0.5),
        ],
    )
    after = out[marks[-3]:]
    check(last_frame(after) == (90, 35), f"{label}: colour 640x480 BR resize -> 90x35 ({last_frame(after)})")

    # A click without dragging changes nothing; 'a' goes back to top-left
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

    # A terminal may deliver one mouse report in pieces (slow link, tmux, ssh):
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

# Real binary with built-in picture: no camera, no fake
REAL = sys.argv[2] if len(sys.argv) > 2 else None
if REAL and os.access(REAL, os.X_OK):
    BIN_SAVE = BIN
    BIN = REAL

    # --image and slideshow checks live in tests/e2e.c
    # --serve checks below still need is kept here: write_png, IARGS and a
    # picture to serve (all of it goes away with --serve)
    import struct as _st
    import zlib as _zl

    A7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]

    def write_png(path, w, h, pix, interlace=0):
        if interlace:
            raw = b""
            for x0, y0, dx, dy in A7:
                for y in range(y0, h, dy):
                    xs = range(x0, w, dx)
                    if xs:
                        raw += b"\x00" + bytes(v for x in xs for v in pix(x, y))
        else:
            raw = b"".join(b"\x00" + bytes(v for x in range(w) for v in pix(x, y)) for y in range(h))

        def ch(t, d):
            return _st.pack(">I", len(d)) + t + d + _st.pack(">I", _zl.crc32(t + d) & 0xFFFFFFFF)

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", _st.pack(">IIBBBBB", w, h, 8, 2, 0, 0, interlace))
                    + ch(b"IDAT", _zl.compress(raw)) + ch(b"IEND", b""))

    idir = tempfile.mkdtemp(prefix="asciicam_img_")
    IARGS = ["-r", "20", "-m", "ascii", "-s", " .:-=+*#%@", "-W", "60", "-H", "40"]
    sdir = os.path.join(idir, "show")
    os.makedirs(sdir)
    write_png(os.path.join(sdir, "a.png"), 16, 16, lambda x, y: (230, 30, 30))

    # --serve: a viewer on TCP gets a clear-screen, then whole frames
    def free_port():
        with socket.socket() as t:
            t.bind(("127.0.0.1", 0))
            return t.getsockname()[1]

    pic = os.path.join(sdir, "a.png")
    port = free_port()
    got = bytearray()

    def viewer():
        for _ in range(40):
            try:
                c = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        else:
            return
        c.settimeout(0.3)
        end = time.time() + 1.5
        while time.time() < end:
            try:
                d = c.recv(65536)
            except TimeoutError:
                continue
            if not d:
                break
            got.extend(d)
        c.close()

    th = threading.Thread(target=viewer)
    th.start()
    _, code, out, _ = run(["--image", pic, "-C", "--serve", str(port)] + IARGS[:-4], 3.0)
    th.join()
    check(code == 0 and bytes(got).startswith(b"\x1b[0m\x1b[?25l\x1b[2J"), "--serve: viewer gets a clear-screen first")
    check(bytes(got).count(b"\x1b[?2026h") >= 5 and bytes(got).count(b"\x1b[?2026h") == bytes(got).count(b"\x1b[?2026l"),
          "--serve: frames arrive whole, one sync pair each")
    check(b"\x1b[38;2;" in bytes(got), "--serve: the colours go to the viewer too")
    check(b"viewer connected" in out, "--serve: the host sees a toast when a viewer joins")

    # a viewer who comes late is brought up to date, and later frames are only
    # changes: a still picture costs one whole picture, not one per frame
    port = free_port()
    first, late = bytearray(), bytearray()

    def viewer2(buf, delay, secs):
        time.sleep(delay)
        for _ in range(40):
            try:
                c = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        else:
            return
        c.settimeout(0.3)
        end = time.time() + secs
        while time.time() < end:
            try:
                d = c.recv(65536)
            except TimeoutError:
                continue
            if not d:
                break
            buf.extend(d)
        c.close()

    t1 = threading.Thread(target=viewer2, args=(first, 0.0, 2.2))
    t2 = threading.Thread(target=viewer2, args=(late, 1.0, 1.4))
    t1.start()
    t2.start()
    _, code, out, _ = run(["--image", pic, "-C", "--serve", str(port)] + IARGS[:-4], 3.5)
    t1.join()
    t2.join()
    one = bytes(first)
    check(len(one) > 1000 and len(one) < 6 * one.find(b"\x1b[?2026l", 100) + 4000,
          "--serve: a still picture is sent once, then only changes (%d bytes)" % len(one))
    two = bytes(late)
    check(b"\x1b[38;2;" in two and two.count(b"\x1b[?2026h") >= 2,
          "--serve: a viewer that comes late gets the whole picture")
    try:
        import pyte
    except ImportError:
        pyte = None
    if pyte:
        def show(data):
            scr = pyte.Screen(200, 100)
            pyte.Stream(scr).feed(data.decode("utf8", "replace"))
            return [r.rstrip() for r in scr.display]

        check(show(one) == show(two) and any(show(two)),
              "--serve: the late viewer ends up with the same picture")

    _, code, out, _ = run(["--serve", "x"] + IARGS, 0.5)
    check(code == 2 and b"--serve needs a port" in out, "--serve with a bad address: usage error")
    _, code, out, _ = run(["--serve"], 0.5)
    check(code == 2 and b"--serve needs an address" in out, "--serve without an address: usage error")

    # --watch: copies what a server sends, quits when it closes
    port = free_port()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)

    def serve_once():
        conn, _ = srv.accept()
        conn.sendall(b"\x1b[2JHELLO-WATCH-FRAME")
        time.sleep(0.4)
        conn.close()

    th = threading.Thread(target=serve_once)
    th.start()
    _, code, out, _ = run(["--watch", "127.0.0.1:%d" % port], 3.0)
    th.join()
    srv.close()
    check(b"HELLO-WATCH-FRAME" in out, "--watch: shows what the server sends")
    check(code == 1 and b"closed the connection" in out, "--watch: says so when the server goes away")

    # --watch --retry: server restarts, viewer comes back
    port = free_port()

    def listener():
        lsock = socket.socket()
        lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        lsock.bind(("127.0.0.1", port))
        lsock.listen(1)
        lsock.settimeout(8)
        return lsock

    # Listening before viewer starts: binding inside thread raced with viewer's
    # first connect, which is refused (exit 1) on a busy machine
    first_listener = listener()

    def restarting_server():
        for n, word in enumerate((b"BEFORE-RESTART", b"AFTER-RESTART")):
            lsock = first_listener if n == 0 else listener()
            try:
                conn, _ = lsock.accept()
            except OSError:
                lsock.close()
                return
            conn.sendall(b"\x1b[2J" + word)
            time.sleep(0.5)
            conn.close()
            lsock.close()
            time.sleep(2.5)  # Nobody listens: viewer has to retry

    th = threading.Thread(target=restarting_server)
    th.start()
    _, code, out, _ = run(["--watch", "127.0.0.1:%d" % port, "--retry"], 7.0)
    th.join()
    check(b"BEFORE-RESTART" in out and b"reconnecting" in out and b"AFTER-RESTART" in out,
          "--watch --retry: reconnects after the server restarts")
    check(code == 0, "--watch --retry: q still quits (%s)" % code)
    check(b"\x1b[?7l" in out and out.rfind(b"\x1b[?7h") > out.rfind(b"\x1b[?7l"),
          "--watch: lines are cut at the edge of a small terminal, wrap is put back on leaving")

    # Captions on local picture (no call): fifo given, y toggles, y makes one
    def drive(args, steps, total, env_extra=None):
        pid, cfd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
            env = dict(os.environ)
            env.update(env_extra or {})
            os.execve(BIN, [BIN] + args, env)
        fcntl.ioctl(cfd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
        out = b""
        t0 = time.monotonic()
        pending = sorted(steps, key=lambda x: x[0])
        while time.monotonic() - t0 < total:
            while pending and time.monotonic() - t0 >= pending[0][0]:
                _, act = pending.pop(0)
                act(cfd)
            r, _, _ = select.select([cfd], [], [], 0.05)
            if r:
                try:
                    out += os.read(cfd, 65536)
                except OSError:
                    break
        os.write(cfd, b"q")
        try:
            end = time.monotonic() + 3
            while time.monotonic() < end:
                r, _, _ = select.select([cfd], [], [], 0.1)
                if r:
                    try:
                        d = os.read(cfd, 65536)
                    except OSError:
                        break
                    if not d:
                        break
                    out += d
        finally:
            _, st = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(st), out

    cdir = tempfile.mkdtemp()
    ffifo = os.path.join(cdir, "cap")
    os.mkfifo(ffifo)
    wf = os.open(ffifo, os.O_RDWR)

    def say(data):
        return lambda cfd: os.write(wf, data)

    def press(k):
        return lambda cfd: os.write(cfd, k)

    code, out = drive(["--demo", "-C", "--captions", ffifo],
                      [(1.0, say(b"~good mor\n")), (1.5, say(b"good morning\n")),
                       (2.5, press(b"y")), (3.2, say(b"hidden words\n")),
                       (4.0, press(b"y")), (4.4, say(b"shown again\n"))], 5.5)
    check(code == 0, "--captions: runs and quits (%s)" % code)
    check(b"good morning" in out and b"\x1b[48;2;20;20;20m" in out and b"\x1b[1m good morning" in out,
          "--captions: a final line is drawn as a bold bar over the picture")
    check(b"\x1b[2m good mor" in out, "--captions: the guess came first, dim")
    check(b"captions off" in out and b"hidden words" not in out,
          "--captions: y hides them (lines still read, not shown)")
    check(b"captions on" in out and b"shown again" in out, "--captions: y shows them again")
    os.close(wf)

    # Picture sent to --watch viewers carries caption bars
    sport = free_port()
    vgot = bytearray()

    def vw():
        cs = socket.socket()
        cs.settimeout(0.3)
        end = time.monotonic() + 4.5
        while time.monotonic() < end:
            try:
                cs.connect(("127.0.0.1", sport))
                break
            except OSError:
                time.sleep(0.1)
        while time.monotonic() < end:
            try:
                d = cs.recv(65536)
            except TimeoutError:
                continue
            if not d:
                break
            vgot.extend(d)
        cs.close()

    sfifo = os.path.join(tempfile.mkdtemp(), "cap")
    os.mkfifo(sfifo)
    swf = os.open(sfifo, os.O_RDWR)
    tv = threading.Thread(target=vw)
    tv.start()
    drive(["--demo", "-C", "--serve", str(sport), "--captions", sfifo],
          [(1.5, lambda cfd: os.write(swf, b"hello viewers\n"))], 4.0)
    tv.join()
    os.close(swf)
    check(b"hello viewers" in bytes(vgot), "--serve: the viewer's picture has the caption bars")

    rdir = tempfile.mkdtemp()
    capfile = os.path.join(rdir, "asciicam-captions")

    def write_default(cfd):
        try:
            fd2 = os.open(capfile, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            return
        os.write(fd2, b"made on demand\n")
        os.close(fd2)

    code, out = drive(["--demo"], [(1.0, press(b"y")), (1.8, write_default)], 3.5,
                      {"XDG_RUNTIME_DIR": rdir})
    import stat as _stat
    check(os.path.exists(capfile) and _stat.S_ISFIFO(os.stat(capfile).st_mode),
          "key y with no source: makes a fifo in the runtime directory")
    check(b"captions on: write lines to " in out and b"made on demand" in out,
          "key y with no source: toast names the fifo, lines written to it show up")

    _, code, out, _ = run(["--demo", "--captions", "/no/such/dir/captions"], 1.0)
    check(code == 1 and b"--captions: cannot open" in out, "--captions: missing path: clear message")

    _, code, out, _ = run(["--watch", "127.0.0.1:%d" % free_port()], 1.0)
    check(code == 1 and b"cannot connect" in out, "--watch: nobody there: clear message")
    _, code, out, _ = run(["--watch", "nonsense"], 1.0)
    check(code == 1 and b"cannot connect" in out, "--watch: bad address: clear message")

    # --room / --call: two people in a room see each other and chat
    def spawn_call(args):
        pid, cfd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
            os.execv(BIN, [BIN] + args)
        fcntl.ioctl(cfd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
        return pid, cfd

    def pump_all(fds, secs):
        got = {x: b"" for x in fds}
        end = time.monotonic() + secs
        while time.monotonic() < end:
            r, _, _ = select.select(fds, [], [], 0.1)
            for x in r:
                try:
                    got[x] += os.read(x, 65536)
                except OSError:
                    pass
        return got

    cport = free_port()
    cap_tmp = tempfile.TemporaryDirectory(prefix="asciicam-captions-")
    capfifo = os.path.join(cap_tmp.name, "captions.fifo")
    os.mkfifo(capfifo)
    room = subprocess.Popen([BIN, "--room", str(cport)], stderr=subprocess.PIPE)
    time.sleep(0.4)
    pa, fa = spawn_call([
        "--call", str(cport), "--name", "Alice", "--demo", "-C",
        "--captions", capfifo,
    ])
    pb, fb = spawn_call(["--call", str(cport), "--name", "Bob", "--demo"])
    first = pump_all([fa, fb], 2.0)
    os.write(fa, b"hello bob\r")
    second = pump_all([fa, fb], 1.5)
    check(b"Alice: hello bob" in second[fb], "--call: chat reaches the other person")
    check(b"you: hello bob" in second[fa], "--call: and shows as yours at home")
    check(b"Alice" in first[fb] + second[fb] and b"\x1b[38;2;" in first[fb] + second[fb],
          "--call: peer name and its colour picture are on screen")

    os.write(fa, b"\x16")  # Ctrl-V: Alice turns her camera off
    off = pump_all([fa, fb], 2.0)
    check(b"^V cam on" in off[fa], "--call: Ctrl-V is shown in the title")
    check(b"kB/s" in off[fa], "--call: the title shows the traffic")
    try:
        import pyte as _pyte
    except ImportError:
        _pyte = None
    if _pyte:
        scr = _pyte.Screen(120, 50)
        _pyte.Stream(scr).feed((first[fb] + second[fb] + off[fb]).decode("utf8", "replace"))
        check(any("camera off" in row for row in scr.display),
              "--call: Bob sees the camera-off card in Alice's tile")
    os.write(fa, b"\x16")
    back = pump_all([fa, fb], 1.0)
    check(b"^V cam off" in back[fa], "--call: Ctrl-V again turns the camera back on")

    # captions: a recogniser writes lines into the fifo, the others read them
    all_bob = first[fb] + second[fb] + off[fb] + back[fb]
    wfd = os.open(capfifo, os.O_RDWR)
    os.write(wfd, b"~good morn\ngood morning bob\n")
    cap = pump_all([fa, fb], 1.5)
    all_bob += cap[fb]
    check(b"you: good morning bob" in cap[fa], "--captions: own caption shows at home")
    if _pyte:
        bscr = _pyte.Screen(120, 50)
        _pyte.Stream(bscr).feed(all_bob.decode("utf8", "replace"))
        check(any("Alice: good morning bob" in row for row in bscr.display),
              "--captions: Bob reads Alice's caption under the pictures")
        check(not any("good morn" in row and "morning" not in row for row in bscr.display),
              "--captions: the partial line was replaced by the final one")
    else:
        check(b"Alice: good morning bob" in cap[fb], "--captions: Bob reads Alice's caption")
    os.write(fb, b"\x14")  # Ctrl-T: Bob hides captions
    hid = pump_all([fa, fb], 1.0)
    all_bob += hid[fb]
    if _pyte:
        bscr = _pyte.Screen(120, 50)
        _pyte.Stream(bscr).feed(all_bob.decode("utf8", "replace"))
        check(not any("good morning bob" in row for row in bscr.display),
              "--captions: Ctrl-T hides them")
    os.close(wfd)

    # nobody reads the screens now: leaving must not hang on a full tty
    os.write(fa, b"\x03")
    os.write(fb, b"\x03")
    cap_tmp.cleanup()
    codes = [os.waitstatus_to_exitcode(os.waitpid(p, 0)[1]) for p in (pa, pb)]
    check(codes == [0, 0], "--call: Ctrl-C leaves cleanly")
    room.terminate()
    rerr = room.communicate()[1]
    check(b"2 in the room" in rerr and b"0 in the room" in rerr, "--room: reports who is in")
    _, code, out, _ = run(["--call", "%d" % free_port(), "--demo"], 1.0)
    check(code == 1 and b"cannot connect" in out, "--call: no room there: clear message")
    _, code, out, _ = run(["--call", str(cport), "--captions", "/no/such/fifo", "--demo"], 1.0)
    check(code == 1 and b"--captions: cannot open" in out, "--captions: missing file: clear message")
    _, code, out, _ = run(["--room", "x"], 0.5)
    check(code == 2 and b"--room needs a port" in out, "--room with a bad address: usage error")
    _, code, out, _ = run(["--room", "7100", "--call", "7100"], 0.5)
    check(code == 2 and b"two programs" in out, "--room with --call: usage error")

    # A still picture costs next to nothing: only changed cells are redrawn
    cport = free_port()
    room = subprocess.Popen([BIN, "--room", str(cport)], stderr=subprocess.PIPE)
    time.sleep(0.4)
    still = ["--image", pic, "-C", "-W", "30", "-H", "12"]
    pa, fa = spawn_call(["--call", str(cport), "--name", "Alice"] + still)
    pb, fb = spawn_call(["--call", str(cport), "--name", "Bob"] + still)
    warm = pump_all([fa, fb], 2.5)
    later = pump_all([fa, fb], 2.0)
    rate = sum(len(v) for v in later.values()) / 2.0
    check(rate < 120000, "--call: a still picture costs %d bytes/s on two screens" % rate)
    try:
        import pyte
    except ImportError:
        pyte = None
    if pyte:
        # what Bob's screen shows of Alice is what Alice sees of herself
        screens = {}
        for who, x in (("a", fa), ("b", fb)):
            scr = pyte.Screen(120, 50)
            pyte.Stream(scr).feed((warm[x] + later[x]).decode("utf8", "replace"))
            screens[who] = scr.display

        def tile(lines, name):
            for i, line in enumerate(lines[:30]):
                if name in line and i > 0:
                    c = line.index(name)
                    return [r[c:c + 30] for r in lines[i + 1:i + 13]]
            return None

        own, seen = tile(screens["a"], "you"), tile(screens["b"], "Alice")
        check(own is not None and own == seen and any(r.strip() for r in own),
              "--call: Bob's screen shows Alice's picture exactly as she sees it")
    os.write(fa, b"\x03")
    os.write(fb, b"\x03")
    for p in (pa, pb):
        os.waitpid(p, 0)
    room.terminate()
    room.communicate()

    _, code, out, _ = run(["--image"], 0.5)
    check(code == 2 and b"--image needs" in out, "--image without a file: usage error")
    shutil.rmtree(idir, ignore_errors=True)

    BIN = BIN_SAVE


# Tone curve: --curve, Curve tab of side panel and its tab strip
def frame_text(buf):
    """Characters of the last picture, escapes removed."""
    i = buf.rfind(SYNC_BEGIN)
    j = buf.find(b"\x1b[?2026l", i)
    seg = buf[i:j if j > 0 else len(buf)]
    return re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seg).decode("utf-8", "replace")


def mean_level(buf, ramp=" .:-=+*#%@"):
    text = frame_text(buf)
    levels = [ramp.index(ch) for ch in text if ch in ramp]
    return sum(levels) / len(levels) if levels else -1


CARGS = ["-d", "fake", "-W", "40", "-H", "20", "-r", "20", "-m", "ascii", "-s", " .:-=+*#%@"]
_, code, plain, _ = run(CARGS, 1.2)
_, code2, white, _ = run(CARGS + ["--curve", "0,255 255,255"], 1.2)
_, code3, black, _ = run(CARGS + ["--curve", "0,0 255,0"], 1.2)
_, code4, same, _ = run(CARGS + ["--curve", "0,0 255,255"], 1.2)
check(code == 0 and code2 == 0 and code3 == 0 and code4 == 0, "--curve: runs and exits 0")
a, w, k, c = mean_level(plain), mean_level(white), mean_level(black), mean_level(same)
check(a > 1 and a < 8, f"the fake picture uses the whole ramp (mean level {a:.2f})")
check(w > 7.5, f"--curve 0,255 255,255 makes everything the brightest character ({w:.2f})")
check(0 <= k < 1.0, f"--curve 0,0 255,0 makes everything the darkest ({k:.2f})")
check(abs(a - c) < 1.0, f"--curve 0,0 255,255 changes nothing ({a:.2f} vs {c:.2f})")
for bad in ("0,0 255", "0,0 100,50", "5,0 255,255", "0,0 99999,5 255,255", "nonsense", "@/nonexistent/c"):
    _, code, out, _ = run(CARGS + ["--curve", bad], 0.5)
    check(code == 2 and b"curve" in out, f"--curve {bad!r} is refused")
_, code, out, _ = run(CARGS + ["--curve"], 0.5)
check(code == 2, "--curve without a value is refused")
cdir = tempfile.mkdtemp(prefix="asciicam_curve_")
cfile = os.path.join(cdir, "c.txt")
with open(cfile, "w") as f:
    f.write("0,255 255,255\n")
_, code, white2, _ = run(CARGS + ["--curve", "@" + cfile], 1.2)
check(code == 0 and mean_level(white2) > 7.5, "--curve @file reads the points from a file")

# panel: k opens Curve tab, [ ] change preset, tab switches tabs
out, marks = session(CARGS, [(b"k", 0.6), (b"]", 0.4), (b"]", 0.4), (b"\t", 0.5), (b"\t", 0.5), (b"k", 0.5)])
check(b"Tone curve" in out[marks[0]:marks[1]], "k opens the curve editor")
check(b"preset: s-curve" in out[marks[1]:marks[2]] and b"preset: asinh" in out[marks[2]:marks[3]],
      "[ and ] switch presets and the panel shows the name")
check(b"Captures" in out[marks[3]:marks[4]], "tab goes to the Files tab")
check(b"Tone curve" in out[marks[4]:marks[5]], "tab again goes back to the curve")
check(b"Tone curve" not in out[marks[6]:], "k closes the panel")
check(b"F" in out and b"C" in out, "tab strip drawn")

# Curve acts on live picture: lift black point to 255 in editor
out, marks = session(CARGS, [(b"k", 0.5)] + [(b"+", 0.03)] * 32 + [(b"k", 0.8)])
check(mean_level(out[marks[0]:marks[1]]) < 8, "editor opens on a normal picture")
check(mean_level(out[marks[-3]:]) > 7.5,
      f"raising the black point to 255 makes the picture white ({mean_level(out[marks[-3]:]):.2f})")

# Tab strip with mouse: open Curve tab, then retract it by clicking it again
out, marks = session(
    CARGS,
    [(mouse(0, 119, 9), 0.2), (mouse(0, 119, 9, True), 0.5),
     (mouse(0, 85, 9), 0.2), (mouse(0, 85, 9, True), 0.6)],
)
check(b"Tone curve" in out[marks[0]:marks[2]], "clicking the Curve tab of the strip opens the panel")
check(b"Tone curve" not in out[marks[3]:marks[5]], "clicking the open tab retracts it")

# Clicking in graph adds a point and dragging moves it; editor reports it
out, marks = session(
    CARGS,
    [(b"k", 0.6), (mouse(0, 95, 8), 0.2), (mouse(32, 104, 3), 0.2), (mouse(0, 104, 3, True), 0.4)],
)
check(b"pt 2/3" in out[marks[1]:], "click in the graph adds a point and selects it")
check(b"frame: 40x20" in out or last_frame(out) in (None, (40, 20)), "the frame was not dragged by clicks on the panel")
shutil.rmtree(cdir, ignore_errors=True)

# ---- Speech recognition: --voice-enroll, --voice-list, --listen, --voice-forget
# Speech is synthesised (a buzz shaped by two moving formants), so this needs no audio files
# and no sound card
import array
import math

_SIN = [math.sin(2 * math.pi * i / 8192) for i in range(8192)]


def synth(segs, tempo=1.0, f0=120.0):
    out = array.array("h")
    n = 0
    for ms, f1, f2 in segs:
        ln = int(ms * tempo * 16)
        for i in range(ln):
            t = (n + i) / 16000.0
            v = 0.0
            h = 1
            while h * f0 < 4500:
                f = h * f0
                w1 = 0.12 * f1 + 60
                w2 = 0.12 * f2 + 60
                a = 1 / (1 + ((f - f1) / w1) ** 2) + 0.7 / (1 + ((f - f2) / w2) ** 2)
                ph = f * t
                v += a * _SIN[int((ph - int(ph)) * 8192) & 8191]
                h += 1
            env = min(1.0, i / 80, (ln - i) / 80)
            out.append(int(v * env * 1500))
        n += ln
    return out


def quiet(ms):
    return array.array("h", [0] * (ms * 16))


def wav_bytes(samples):
    data = samples.tobytes()
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16) +
            b"data" + struct.pack("<I", len(data)) + data)


VW = {
    "alpha": [(120, 300, 2200), (150, 700, 1200), (120, 400, 800)],
    "bravo": [(150, 600, 1000), (120, 300, 2500), (150, 500, 1500)],
}
VX = [(160, 900, 1700), (160, 250, 3200), (140, 750, 2400)]  # never taught

vdir = tempfile.mkdtemp(prefix="asciicam-voice-")
vfile = os.path.join(vdir, "sub", "voice.tpl")


def vrun(args, stdin=None):
    return subprocess.run([BIN, "--voice", vfile] + args, input=stdin,
                          capture_output=True, timeout=60)


r = vrun(["--voice-list"])
check(r.returncode == 0 and b"0 words" in r.stderr, "an empty voice file lists nothing")

for name, segs in VW.items():
    for tempo, f0 in ((1.0, 120), (0.85, 105), (1.2, 135)):
        wp = os.path.join(vdir, f"{name}.wav")
        with open(wp, "wb") as fh:
            fh.write(wav_bytes(quiet(300) + synth(segs, tempo, f0) + quiet(500)))
        r = vrun(["--voice-enroll", name, wp])
        check(r.returncode == 0 and b"1 new" in r.stderr, f"enrol {name} {tempo}: {r.stderr!r}")

check(os.path.exists(vfile), "the voice file was created, with its directories")
r = vrun(["--voice-list"])
check(b"2 words, 6 templates" in r.stderr and b"alpha" in r.stderr and b"bravo" in r.stderr,
      f"list shows both words: {r.stderr!r}")

# Enrolling from stdin ("-"), a raw PCM stream
r = vrun(["--voice-enroll", "alpha", "-"], stdin=(quiet(300) + synth(VW["alpha"], 0.95, 125) + quiet(500)).tobytes())
check(r.returncode == 0 and b"4 of 5" in r.stderr, f"enrol from stdin: {r.stderr!r}")

# A sentence: known, unknown, known, known. Said at other speeds and pitch
said = (quiet(600) + synth(VW["bravo"], 0.92, 112) + quiet(700) + synth(VX, 1.0, 120) +
        quiet(700) + synth(VW["alpha"], 1.1, 128) + quiet(700) + synth(VW["bravo"], 1.0, 118) + quiet(600))
pcm_path = os.path.join(vdir, "said.pcm")
with open(pcm_path, "wb") as fh:
    fh.write(said.tobytes())
wav_path = os.path.join(vdir, "said.wav")
with open(wav_path, "wb") as fh:
    fh.write(wav_bytes(said))

for label, path in (("raw pcm", pcm_path), ("wav", wav_path)):
    r = vrun(["--listen", path])
    lines = r.stdout.decode().split()
    finals = [x for x in lines if not x.startswith("~")]
    check(r.returncode == 0, f"--listen {label} ends at the end of the file")
    check(finals == ["bravo", "alpha", "bravo"],
          f"{label}: the three taught words in order, the unknown one skipped ({lines})")
    check(any(x.startswith("~") for x in lines), f"{label}: hypotheses come before finals")

# The same from a pipe on stdin
r = vrun(["--listen", "-"], stdin=said.tobytes())
check(r.stdout.decode().split().count("bravo") >= 2, "--listen - reads stdin")

# A live fifo, written at about real time, is followed and gives lines as it goes
fifo = os.path.join(vdir, "mic.fifo")
os.mkfifo(fifo)
proc = subprocess.Popen([BIN, "--voice", vfile, "--listen", fifo], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(0.3)
fd = os.open(fifo, os.O_WRONLY)
data = said.tobytes()
got = []
rd_done = threading.Event()


def reader():
    for ln in proc.stdout:
        got.append((time.time(), ln.decode().strip()))
    rd_done.set()


threading.Thread(target=reader, daemon=True).start()
t_start = time.time()
chunk = 3200
for i in range(0, len(data), chunk):
    os.write(fd, data[i:i + chunk])
    time.sleep(0.05)  # twice real time: the recogniser does not care
time.sleep(1.0)
check(proc.poll() is None, "--listen on a fifo keeps running when the writer pauses")
os.close(fd)
time.sleep(0.4)
check(proc.poll() is None, "and when the writer closes it")
proc.terminate()
proc.wait(timeout=5)
f_live = [t for _, t in got if not t.startswith("~")]
check(f_live == ["bravo", "alpha", "bravo"], f"live fifo gives the words: {f_live}")

# Not usable: nothing enrolled, a bad file, a bad format
r = subprocess.run([BIN, "--voice", os.path.join(vdir, "none.tpl"), "--listen", pcm_path], capture_output=True, timeout=30)
check(r.returncode == 1 and b"nothing enrolled" in r.stderr, f"nothing enrolled is said: {r.stderr!r}")
r = vrun(["--listen", os.path.join(vdir, "absent.pcm")])
check(r.returncode == 1 and b"cannot open" in r.stderr, "missing audio file is an error")
bad8 = wav_bytes(quiet(200)).replace(struct.pack("<HH", 2, 16), struct.pack("<HH", 1, 8))
with open(os.path.join(vdir, "bad8.wav"), "wb") as fh:
    fh.write(bad8)
r = vrun(["--listen", os.path.join(vdir, "bad8.wav")])
check(r.returncode == 1 and b"16 bit" in r.stderr, f"8 bit audio is refused: {r.stderr!r}")
r = vrun(["--voice-enroll", "x", os.path.join(vdir, "bad8.wav")])
check(r.returncode == 1, "and so is enrolling it")
silent = os.path.join(vdir, "silent.wav")
with open(silent, "wb") as fh:
    fh.write(wav_bytes(quiet(2000)))
r = vrun(["--voice-enroll", "x", silent])
check(r.returncode == 1 and b"no speech" in r.stderr, "enrolling silence says so")
r = subprocess.run([BIN, "--voice-enroll", "x"], capture_output=True, timeout=10)
check(r.returncode == 2 and b"needs" in r.stderr, "--voice-enroll needs two values")
r = subprocess.run([BIN, "--listen"], capture_output=True, timeout=10)
check(r.returncode == 2, "--listen needs a value")

# Forget
r = vrun(["--voice-forget", "alpha"])
check(r.returncode == 0 and b"forgot" in r.stderr, "forget alpha")
r = vrun(["--voice-forget", "alpha"])
check(r.returncode == 1 and b"not enrolled" in r.stderr, "forgetting twice is reported")
r = vrun(["--listen", pcm_path])
check(r.stdout.decode().split().count("alpha") == 0 and "bravo" in r.stdout.decode(),
      "a forgotten word is no longer heard")

# Default location comes from XDG_CONFIG_HOME
env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(vdir, "xdg"))
wp = os.path.join(vdir, "alpha.wav")
with open(wp, "wb") as fh:
    fh.write(wav_bytes(quiet(300) + synth(VW["alpha"]) + quiet(500)))
r = subprocess.run([BIN, "--voice-enroll", "alpha", wp], env=env, capture_output=True, timeout=30)
check(os.path.exists(os.path.join(vdir, "xdg", "asciicam", "voice.tpl")), f"default path under XDG_CONFIG_HOME: {r.stderr!r}")
shutil.rmtree(vdir, ignore_errors=True)

# Sentences from a neural model (--listen --model)
# Weights are tiny random model of unit tests, so words are noise: what is checked is that
# eal binary loads weights + tokenizer.json + config.json, cuts audio into utterances and
# prints one line per sentence
try:
    import numpy  # noqa: F401  (gen_asrnet_vectors needs it)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
    import gen_asrnet_vectors as gmv
except Exception as exc:  # numpy missing: skip
    gmv = None
    print("skip: --model e2e needs numpy:", exc)
if gmv is not None:
    mdir = tempfile.mkdtemp(prefix="asciicam-model-")
    with open(os.path.join(mdir, "model.safetensors"), "wb") as fh:
        fh.write(gmv.safetensors_bytes(gmv.gen_weights(gmv.CFG)))
    with open(os.path.join(mdir, "tokenizer.json"), "w") as fh:
        fh.write('{"model":{"vocab":{"<pad>":0,"<s>":1,"</s>":2'
                 + "".join(',"\u2581w%d":%d' % (i, i) for i in range(3, 50)) + "}}}")
    cfg = {"encoder_config": {"hidden_size": 32, "intermediate_size": 64, "num_hidden_layers": 3,
                             "num_attention_heads": 2, "sliding_windows": [[5, 4], [5, 0], [4, 3]]},
           "vocab_size": 50, "hidden_size": 32, "intermediate_size": 48, "num_hidden_layers": 2,
           "num_attention_heads": 2, "bos_token_id": 1, "eos_token_id": 2}
    with open(os.path.join(mdir, "config.json"), "w") as fh:
        json.dump(cfg, fh)
    model = os.path.join(mdir, "model.safetensors")

    def talk(ms):
        out = []
        for i in range(ms * 16):
            t = i / 16000
            out.append(int(6000 * math.sin(2 * math.pi * 180 * t) + 3000 * math.sin(2 * math.pi * 540 * t)
                           + random.randint(-1000, 1000)))
        return array.array("h", out)

    random.seed(5)
    sentences = quiet(800) + talk(2500) + quiet(1000) + talk(1500) + quiet(900)
    wavp = os.path.join(mdir, "s.wav")
    with open(wavp, "wb") as fh:
        fh.write(wav_bytes(sentences))
    r = subprocess.run([BIN, "--listen", wavp, "--model", model], capture_output=True, timeout=60)
    lines = r.stdout.decode().splitlines()
    finals = [x for x in lines if not x.startswith("~")]
    check(r.returncode == 0 and len(finals) == 2 and all(x.startswith("w") for x in finals),
          f"--model: two sentences from two utterances: {lines!r} {r.stderr!r}")
    r = subprocess.run([BIN, "--listen", "-", "--model", model], input=wav_bytes(sentences),
                       capture_output=True, timeout=60)
    check(r.stdout.decode().splitlines() == lines, "--model: stdin gives the same lines")
    # a wrong config is refused with the reason
    with open(os.path.join(mdir, "config.json"), "w") as fh:
        fh.write('{"hidden_size": 33}')
    r = subprocess.run([BIN, "--listen", wavp, "--model", model], capture_output=True, timeout=30)
    check(r.returncode == 1 and b"--model" in r.stderr and r.stdout == b"", f"bad config refused: {r.stderr!r}")
    os.remove(os.path.join(mdir, "config.json"))  # no config: tiny shape, which these weights are not
    r = subprocess.run([BIN, "--listen", wavp, "--model", model], capture_output=True, timeout=30)
    check(r.returncode == 1 and b"wrong shape" in r.stderr, f"shape mismatch named: {r.stderr!r}")
    r = subprocess.run([BIN, "--listen", wavp, "--model", os.path.join(mdir, "none.safetensors")],
                       capture_output=True, timeout=30)
    check(r.returncode == 1 and b"cannot open" in r.stderr, "missing weights")
    shutil.rmtree(mdir, ignore_errors=True)


print("e2e:", "FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)

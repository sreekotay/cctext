#!/usr/bin/env python3
"""End-to-end TUI checks: drive bin/cctext in a pseudo-terminal.

    python3 tests/tui_pty_test.py [path/to/cctext] [case ...]

Each case opens a scratch file, feeds raw terminal bytes, saves with
Ctrl-S, quits with Ctrl-Q, and checks the bytes on disk (and, where it
matters, the tty state or the screen). No pyte needed for the byte
checks; screen checks use pyte when it is installed and skip otherwise.
Exit status is the number of failed cases.
"""
import errno
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time

try:
    import pyte  # optional: screen checks
except ImportError:  # pragma: no cover
    pyte = None

ROWS, COLS = 24, 80

if pyte is not None:
    def _pyte_delete_lines(self, count=None):
        # pyte 0.8 keeps a line in place when the line `count` below it
        # was never written (absent from the buffer); a terminal shows a
        # blank one. Region scrolls (DECSTBM + DL) depend on it.
        count = count or 1
        top, bottom = self.margins or pyte.screens.Margins(0, self.lines - 1)
        if top <= self.cursor.y <= bottom:
            self.dirty.update(range(self.cursor.y, self.lines))
            for y in range(self.cursor.y, bottom + 1):
                if y + count <= bottom and y + count in self.buffer:
                    self.buffer[y] = self.buffer.pop(y + count)
                else:
                    self.buffer.pop(y, None)
            self.carriage_return()
    pyte.Screen.delete_lines = _pyte_delete_lines


def strip_strings(b):
    """Drop ESC _ .. ST, ESC P .. ST and ESC ] 1337 .. BEL/ST strings (the
    pictures and the terminal queries) so pyte sees only the text."""
    out = bytearray()
    i, n = 0, len(b)
    while i < n:
        if b[i] == 0x1b and i + 1 < n and (b[i + 1] in b"_P" or
                                           b[i + 1:i + 6] == b"]1337"):
            j = i + 2
            while j < n:
                if b[j] == 0x07 and b[i + 1] == 0x5d:
                    j += 1
                    break
                if b[j] == 0x1b and j + 1 < n and b[j + 1] == 0x5c:
                    # tmux passthrough doubles inner ESCs: ESC ESC \ is
                    # not the end.
                    if b[i + 1] == 0x50 and b[i + 2:i + 7] == b"tmux;" and \
                            b[j - 1] == 0x1b:
                        j += 2
                        continue
                    j += 2
                    break
                j += 1
            i = j
            continue
        out.append(b[i])
        i += 1
    return bytes(out)


def sixel_decode(data):
    """A sixel DCS body (after ESC P .. q, up to ST) -> (w, h, {(x, y): rgb})."""
    import re
    pal = {}
    px = {}
    x = y = 0
    cur = 0
    w = h = 0
    i, n = 0, len(data)
    while i < n:
        c = data[i]
        if c == ord('"'):
            m = re.match(rb'"(\d+);(\d+);(\d+);(\d+)', data[i:])
            if m:
                w, h = int(m.group(3)), int(m.group(4))
                i += m.end()
                continue
            i += 1
        elif c == ord('#'):
            m = re.match(rb'#(\d+)(?:;(\d+);(\d+);(\d+);(\d+))?', data[i:])
            cur = int(m.group(1))
            if m.group(2):
                pal[cur] = tuple(int(int(m.group(k)) * 255 / 100 + 0.5) for k in (3, 4, 5))
            i += m.end()
        elif c == ord('!'):
            m = re.match(rb'!(\d+)(.)', data[i:], re.S)
            cnt, ch = int(m.group(1)), m.group(2)[0]
            for _ in range(cnt):
                bits = ch - 63
                for k in range(6):
                    if bits & (1 << k):
                        px[(x, y + k)] = pal.get(cur, (0, 0, 0))
                x += 1
            i += m.end()
        elif c == ord('$'):
            x = 0
            i += 1
        elif c == ord('-'):
            x = 0
            y += 6
            i += 1
        elif 63 <= c <= 126:
            bits = c - 63
            for k in range(6):
                if bits & (1 << k):
                    px[(x, y + k)] = pal.get(cur, (0, 0, 0))
            x += 1
            i += 1
        else:
            i += 1
    return w, h, px


class FakeTerm:
    """A terminal on the far side of the pty for the picture cases: it
    answers the startup queries as a terminal of kind `mode` would ('kitty',
    'sixel', 'iterm', or 'none' = answers DA1 only; 'silent' answers
    nothing), optionally inside tmux (passthrough = the echo comes back),
    and records every graphics command (kitty APC, with t=t temp files
    read and deleted as kitty does)."""

    def __init__(self, mode, cell=(10, 20), truecolor=True, tmux=None, bg=None, bel=False):
        self.mode, self.cell, self.truecolor, self.tmux = mode, cell, truecolor, tmux
        # OSC 11 (the background): bg is the X11 colour spec to answer
        # (b"rgb:ffff/ffff/ffff"), None = no answer; bel ends it with BEL.
        self.bg, self.bel = bg, bel
        self.buf = b""
        self.files = {}   # t=t path -> bytes read
        self.cmds = []    # kitty commands: dict of keys (+ 'payload')

    def _kitty(self, body, wrapped):
        import base64
        keys, _, payload = body.partition(b";")
        kv = dict(p.split(b"=", 1) for p in keys.split(b",") if b"=" in p)
        kv = {k.decode(): v.decode() for k, v in kv.items()}
        kv["payload"] = payload
        kv["tmux"] = wrapped
        self.cmds.append(kv)
        if kv.get("t") == "t" and payload:
            path = base64.b64decode(payload).decode()
            try:
                with open(path, "rb") as f:
                    self.files[path] = f.read()
                os.unlink(path)
            except OSError:
                self.files[path] = None
        if kv.get("a") == "q" and self.mode == "kitty":
            if kv.get("t") == "t" and self.files.get(
                    __import__("base64").b64decode(payload).decode()) is None:
                return b"\x1b_Gi=%s;ENOENT\x1b\\" % kv["i"].encode()
            return b"\x1b_Gi=%s;OK\x1b\\" % kv["i"].encode()
        return b""

    def replies(self, chunk):
        self.buf += chunk
        out = b""
        b = self.buf
        i = 0
        while True:
            j = b.find(b"\x1b", i)
            if j < 0:
                i = len(b)
                break
            if j + 1 >= len(b):
                i = j
                break
            k = b[j + 1:j + 2]
            if k in (b"_", b"P", b"]"):
                end = j + 2
                while True:
                    e = b.find(b"\x1b\\", end)
                    if e < 0:
                        break
                    if b[j + 2:j + 7] == b"tmux;" and b[e - 1:e] == b"\x1b":
                        end = e + 2
                        continue
                    break
                if k == b"]":
                    bel = b.find(b"\x07", j)
                    if bel >= 0 and (e < 0 or bel < e):
                        i = bel + 1
                        continue
                if e < 0:
                    i = j
                    break
                body = b[j + 2:e]
                i = e + 2
                if k == b"_" and body[:1] == b"G":
                    out += self._kitty(body[1:], False)
                elif k == b"P" and body.startswith(b"tmux;"):
                    inner = body[5:].replace(b"\x1b\x1b", b"\x1b")
                    if self.tmux == "passthrough":
                        out += self.replies_inner(inner)
                elif k == b"P" and body == b"$qm":
                    if self.truecolor and self.mode != "silent":
                        out += b"\x1bP1$r0;48:2:1:2:3m\x1b\\"
                elif k == b"]" and body == b"11;?" and self.bg is not None:
                    out += b"\x1b]11;" + self.bg + (b"\x07" if self.bel else b"\x1b\\")
                continue
            if k == b"[":
                m = __import__("re").match(rb"\x1b\[([?>]?)([\d;]*)([a-zA-Z])", b[j:])
                if not m:
                    if len(b) - j < 16:
                        i = j
                        break
                    i = j + 2
                    continue
                i = j + m.end()
                pre, par, fin = m.group(1), m.group(2), m.group(3)
                if self.mode == "silent":
                    continue
                if fin == b"c" and pre == b"" and par in (b"", b"0"):
                    if self.tmux:
                        out += b"\x1b[?1;2;4c" if self.mode == "sixel" else b"\x1b[?1;2c"
                    else:
                        out += b"\x1b[?62;4;22c" if self.mode == "sixel" else b"\x1b[?62;22c"
                elif fin == b"q" and pre == b">" and not self.tmux:
                    out += self._xtversion()
                elif fin == b"t" and par == b"16":
                    out += b"\x1b[6;%d;%dt" % (self.cell[1], self.cell[0])
                elif fin == b"S" and pre == b"?" and self.mode == "sixel":
                    out += b"\x1b[?1;0;256S"
                continue
            i = j + 1
        self.buf = b[i:]
        return out

    def _xtversion(self):
        name = {"kitty": b"kitty(0.35.2)", "iterm": b"iTerm2 3.5.0",
                "sixel": b"foot(1.17.2)"}.get(self.mode, b"xterm(390)")
        return b"\x1bP>|" + name + b"\x1b\\"

    def replies_inner(self, inner):
        # A query that went through tmux's passthrough: the outer terminal.
        out = b""
        if inner.startswith(b"\x1b_G") and inner.endswith(b"\x1b\\"):
            out += self._kitty(inner[3:-2], True)
        elif inner == b"\x1b[>0q":
            out += self._xtversion()
        return out

    def images(self):
        """kitty images alive at the end: id -> (w, h, c, r, pixels)."""
        import base64
        import zlib
        live, acc = {}, None
        for c in self.cmds:
            a = c.get("a")
            if a == "d":
                if "i" in c:
                    live.pop(int(c["i"]), None)
                continue
            if a in ("T", "t") and "i" in c:
                acc = dict(c)
                acc["data"] = b""
            elif a is not None or acc is None:
                continue
            if acc.get("t") == "t":
                path = base64.b64decode(c["payload"]).decode()
                acc["data"] = self.files.get(path) or b""
            else:
                acc["data"] += base64.b64decode(c["payload"])
            if c.get("m", "0") == "0":
                data = acc["data"]
                if acc.get("o") == "z":
                    data = zlib.decompress(data)
                live[int(acc["i"])] = (int(acc["s"]), int(acc["v"]), int(acc.get("c", 0)),
                                       int(acc.get("r", 0)), data)
                acc = None
        return live

    def deletes(self):
        return [int(c["i"]) for c in self.cmds if c.get("a") == "d" and "i" in c]

    def sent(self):
        return [int(c["i"]) for c in self.cmds if c.get("a") in ("T", "t") and "i" in c]


if pyte is not None:
    class FakeScreen(pyte.Screen):
        """pyte plus a pixel layer: a sixel (or an iTerm2 image, as a solid
        marker) paints its cells at the cursor; text written over a cell,
        EL / ED and line moves erase or move those pixels, as terminals
        that keep images in cells do. fg keeps the 256-colour index
        (kitty placeholders carry the image id there)."""

        def __init__(self, cols, rows, cell):
            super().__init__(cols, rows)
            self.cw, self.ch = cell
            self.pix = {}   # (cx, cy) -> {(dx, dy): rgb}
            self.nimg = 0

        def select_graphic_rendition(self, *attrs, **kw):
            super().select_graphic_rendition(*attrs, **kw)
            a = list(attrs)
            for k in range(len(a) - 2):
                if a[k] == 38 and a[k + 1] == 5:
                    self.cursor.attrs = self.cursor.attrs._replace(fg="i%d" % a[k + 2])

        def _clear(self, y, x0, x1):
            for x in range(x0, x1):
                self.pix.pop((x, y), None)

        def draw(self, data):
            y, x0 = self.cursor.y, self.cursor.x
            super().draw(data)
            x1 = self.cursor.x if self.cursor.y == y else self.columns
            self._clear(y, x0, max(x1, x0 + 1))

        def erase_in_line(self, how=0, private=False):
            y, x = self.cursor.y, self.cursor.x
            super().erase_in_line(how, private)
            if how == 0:
                self._clear(y, x, self.columns)
            elif how == 1:
                self._clear(y, 0, x + 1)
            else:
                self._clear(y, 0, self.columns)

        def erase_in_display(self, how=0, *args, **kw):
            super().erase_in_display(how, *args, **kw)
            if how in (2, 3):
                self.pix = {}

        def _move(self, count, up):
            top, bot = self.margins or pyte.screens.Margins(0, self.lines - 1)
            y = self.cursor.y
            if not (top <= y <= bot):
                return
            new = {}
            for (cx, cy), v in self.pix.items():
                if cy < y or cy > bot:
                    new[(cx, cy)] = v
                    continue
                ny = cy - count if up else cy + count
                if (up and cy - count < y) or ny > bot or ny < y:
                    continue
                new[(cx, ny)] = v
            self.pix = new

        def delete_lines(self, count=None):
            super().delete_lines(count)
            self._move(count or 1, True)

        def insert_lines(self, count=None):
            super().insert_lines(count)
            self._move(count or 1, False)

        def image(self, px, w, h):
            x0, y0 = self.cursor.x, self.cursor.y
            for (x, y), rgb in px.items():
                if x >= w or y >= h:
                    continue
                cx, cy = x0 + x // self.cw, y0 + y // self.ch
                if cx >= self.columns or cy >= self.lines:
                    continue
                self.pix.setdefault((cx, cy), {})[(x % self.cw, y % self.ch)] = rgb
            self.nimg += 1

    def fake_screen(out, cols, rows, cell=(10, 20)):
        """Feed the output in order: text to pyte, pictures to the pixel
        layer at the cursor."""
        sc = FakeScreen(cols, rows, cell)
        st = pyte.ByteStream(sc)
        b = bytes(out)
        i, n = 0, len(b)
        while i < n:
            j = b.find(b"\x1b", i)
            while j >= 0 and j + 1 < n and b[j + 1:j + 2] not in (b"_", b"P", b"]"):
                j = b.find(b"\x1b", j + 1)
            if j < 0:
                st.feed(b[i:])
                break
            st.feed(b[i:j])
            k = b[j + 1:j + 2]
            e = b.find(b"\x07" if k == b"]" else b"\x1b\\", j)
            if k == b"]" and b[j:j + 6] != b"\x1b]1337":
                e2 = b.find(b"\x1b\\", j)
                e = min(x for x in (e, e2) if x >= 0) if (e >= 0 or e2 >= 0) else -1
            if e < 0:
                break
            body = b[j + 2:e]
            i = e + (1 if b[e:e + 1] == b"\x07" else 2)
            if k == b"P" and body.startswith(b"tmux;"):
                continue
            if k == b"P":
                q = body.find(b"q")
                if q >= 0 and all(c in b"0123456789;" for c in body[:q]):
                    w, h, px = sixel_decode(body[q + 1:])
                    sc.image(px, w, h)
            elif k == b"]" and body.startswith(b"1337;File="):
                import re
                m = re.search(rb"width=(\d+);height=(\d+)", body)
                if m:
                    w, h = int(m.group(1)) * cell[0], int(m.group(2)) * cell[1]
                    mark = hash(body) & 0xffffff
                    px = {(x, y): (mark >> 16, (mark >> 8) & 255, mark & 255)
                          for x in range(0, w, cell[0]) for y in range(0, h, cell[1])}
                    sc.image(px, w, h)
        return sc


class Tui:
    """jobctl: run the editor as a foreground job below a waiting parent,
    the way a shell does, so SIGTSTP can stop it (a session leader's own
    group is orphaned and the kernel drops stop signals to it)."""

    def __init__(self, exe, args, env_extra=None, rows=ROWS, cols=COLS,
                 jobctl=False, fake=None):
        self.rows, self.cols = rows, cols
        self.out = bytearray()
        self.fake = fake
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(os.environ)
            env["TERM"] = "xterm-256color"
            for k in ("TMUX", "TERM_PROGRAM", "WAYLAND_DISPLAY", "DISPLAY",
                      "SSH_TTY", "SSH_CONNECTION", "COLORFGBG"):
                env.pop(k, None)
            if env_extra:
                env.update(env_extra)
            if jobctl:
                g = os.fork()
                if g == 0:
                    os.setpgid(0, 0)
                    signal.signal(signal.SIGTTOU, signal.SIG_IGN)
                    os.tcsetpgrp(0, os.getpgrp())
                    signal.signal(signal.SIGTTOU, signal.SIG_DFL)
                    os.execve(exe, [exe] + list(args), env)
                _, st = os.waitpid(g, 0)
                os._exit(os.WEXITSTATUS(st) if os.WIFEXITED(st) else 128)
            os.execve(exe, [exe] + list(args), env)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.status = None
        self.target = pid
        if jobctl:
            end = time.time() + 3.0
            while time.time() < end:
                try:
                    with open("/proc/%d/task/%d/children" % (pid, pid)) as f:
                        kids = f.read().split()
                except OSError:
                    kids = []
                if kids:
                    self.target = int(kids[0])
                    break
                time.sleep(0.02)

    def pump(self, secs=0.3):
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError as e:
                if e.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            self.out += chunk
            if self.fake is not None:
                rep = self.fake.replies(chunk)
                if rep:
                    os.write(self.fd, rep)

    def send(self, data, settle=0.25):
        if isinstance(data, str):
            data = data.encode()
        try:
            os.write(self.fd, data)
        except OSError as e:
            # macOS: EIO once the child has exited (a quit key before this one).
            if e.errno != errno.EIO:
                raise
            return
        self.pump(settle)

    def wait_exit(self, secs=5.0):
        end = time.time() + secs
        while time.time() < end:
            self.pump(0.05)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = st
                return st
        return None

    def kill(self):
        if self.target != self.pid:
            try:
                os.kill(self.target, signal.SIGKILL)
            except OSError:
                pass
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
        try:
            os.close(self.fd)
        except OSError:
            pass

    def screen(self):
        if pyte is None:
            return None
        sc = pyte.Screen(self.cols, self.rows)
        st = pyte.ByteStream(sc)
        # pyte prints APC / DCS / OSC 1337 payloads (kitty graphics, sixel,
        # iTerm2 images, the startup queries) as text; a terminal does not.
        st.feed(strip_strings(bytes(self.out)))
        return sc


FAILS = []
PREFIX = [""]  # "light: " while a case re-runs in the light theme


def check(cond, name, detail=""):
    name = PREFIX[0] + name
    if not cond:
        FAILS.append(name)
        print("FAIL: %s %s" % (name, detail))
    else:
        print("ok:   %s" % name)


def scratch_file(tmp, name, body=b""):
    p = os.path.join(tmp, name)
    with open(p, "wb") as f:
        f.write(body)
    return p


def edit_session(exe, tmp, feed, body=b"", name="t.txt", settle=0.5):
    """Open `name`, send each chunk of `feed`, save, quit; return file bytes."""
    path = scratch_file(tmp, name, body)
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        for chunk in feed:
            if isinstance(chunk, (int, float)):
                t.pump(chunk)
            elif isinstance(chunk, tuple):
                t.send(chunk[0], chunk[1])
            else:
                t.send(chunk, settle)
        t.send(b"\x13", 0.4)  # Ctrl-S
        t.send(b"\x11", 0.2)  # Ctrl-Q
        t.wait_exit(5.0)
    finally:
        t.kill()
    with open(path, "rb") as f:
        return f.read(), t


PASTE = b"if (a<3) arr[I] = x[O];\r\n\tb = c<4;\rend\r\n"
PASTE_WANT = b"if (a<3) arr[I] = x[O];\n\tb = c<4;\nend\n"


def case_bracketed_paste(exe, tmp):
    got, t = edit_session(exe, tmp, [b"\x1b[200~" + PASTE + b"\x1b[201~"])
    check(got == PASTE_WANT, "bracketed paste is literal", repr(got))
    check(b"\x1b[?2004h" in t.out, "bracketed paste enabled")
    check(b"\x1b[?2004l" in t.out, "bracketed paste disabled on exit")


def case_paste_split_reads(exe, tmp):
    # Opener, body and terminator arrive in separate reads (a few ms apart,
    # inside the ESC wait), each split mid-sequence.
    data = b"\x1b[200~" + PASTE + b"\x1b[201~"
    feed = [(data[:3], 0.01), (data[3:20], 0.01), (data[20:-3], 0.01),
            data[-3:]]
    got, _ = edit_session(exe, tmp, feed, name="split.txt")
    check(got == PASTE_WANT, "bracketed paste split across reads", repr(got))


def case_paste_one_undo(exe, tmp):
    got, _ = edit_session(
        exe, tmp, [b"x", b"\x1b[200~" + PASTE + b"\x1b[201~", b"\x1a"],
        name="undo.txt")
    check(got == b"x", "paste is one undo record", repr(got))


def case_unbracketed_typing(exe, tmp):
    # A terminal without ?2004 (or a fast typist) sends plain bytes in one
    # burst. `<3` and `[I` are text, not mouse / focus reports.
    got, _ = edit_session(exe, tmp, [b"a<3) q[I] r[O]"], name="typed.txt")
    check(got == b"a<3) q[I] r[O]", "typed <digit and [I stay text", repr(got))


def case_dangling_csi(exe, tmp):
    # ESC [ 1 ; then nothing: the timeout drops the CSI, it is not typed.
    got, _ = edit_session(exe, tmp, [b"k", b"\x1b[1;", 0.4, b"z"],
                          name="dangle.txt")
    check(got == b"kz", "dangling CSI is discarded", repr(got))


def case_page_keys(exe, tmp):
    body = b"".join(b"line %d\n" % i for i in range(200))
    # PageDown twice, then type a marker at the caret's line start.
    got, _ = edit_session(exe, tmp, [b"\x1b[6~", b"\x1b[6~", b"\x1b[H", b"#"],
                          body=body, name="page.txt")
    lines = got.split(b"\n")
    hit = [i for i, l in enumerate(lines) if l.startswith(b"#")]
    check(len(hit) == 1 and hit[0] >= 30, "PageDown moves a page",
          repr(hit))
    got2, _ = edit_session(exe, tmp, [b"\x1b[1;5F", b"#"], body=body,
                           name="cend.txt")
    check(got2.endswith(b"#"), "Ctrl-End goes to EOF", repr(got2[-12:]))


def case_last_line(exe, tmp):
    """The caret onto the empty line after a final newline (Down, Ctrl-End,
    PageDown) and the wheel to EOF: the pane keeps the document's end on
    screen instead of going blank ('~' rows only). Wrap on and off, Rich
    and Source, with and without the final newline."""
    if pyte is None:
        print("skip: last line (no pyte)")
        return
    body = b"".join(b"line %d of some text here\n" % i for i in range(1, 60))
    src = os.path.join(tmp, "source.json")
    with open(src, "w") as f:
        f.write('{"rich": false}')
    moves = {"Down": b"\x1b[B" * 59, "Ctrl-End": b"\x1b[1;5F",
             "PageDown": b"\x1b[6~" * 5, "wheel": b"\x1b[<65;10;5M" * 40}
    for nl in (1, 0):
        for mode in ("rich wrap", "rich nowrap", "source wrap"):
            for how, seq in moves.items():
                args = ["--settings", src] if "source" in mode else []
                name = "last_%d_%s_%s.txt" % (nl, mode.replace(" ", "_"), how)
                t, _ = open_tui(exe, tmp, name, body if nl else body[:-1], args=args)
                try:
                    if "nowrap" in mode:
                        t.send(b"\x1bm", 0.2)
                    t.send(seq, 0.6)
                    rows = (screen_text(t) or "").split("\n")
                    t.send(b"\x11", 0.2)
                    t.wait_exit(5.0)
                finally:
                    t.kill()
                text = [r for r in rows[:-1] if r.strip() and not r.strip().startswith("~")]
                want = 1 if how == "wheel" else len(rows) // 2
                check(len(text) >= want and any("line 59" in r for r in text),
                      "last line: %s to EOF keeps text on screen (%s, %s)" %
                      (how, mode, "final newline" if nl else "no final newline"),
                      "%d text rows: %r" % (len(text), rows[:2]))


def case_mouse_click(exe, tmp):
    body = b"".join(b"line %d\n" % i for i in range(40))
    # Click row 3 (line index 2), release, type.
    got, _ = edit_session(exe, tmp, [b"\x1b[<0;12;3M\x1b[<0;12;3m", b"#"],
                          body=body, name="click.txt")
    lines = got.split(b"\n")
    check(b"#" in lines[2], "SGR click places the caret", repr(lines[:4]))


def screen_text(t):
    sc = t.screen()
    if sc is None:
        return None
    return "\n".join(sc.display)


def case_find_run(exe, tmp):
    body = b"".join(b"line %d\n" % i for i in range(40))
    path = scratch_file(tmp, "find.txt", body)
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        t.send(b"\x06", 0.3)          # Ctrl-F
        t.send(b"line 3", 0.5)        # one burst: a typed run
        txt = screen_text(t)
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if txt is None:
        print("skip: find run (no pyte)")
        return
    check("find: line 3" in txt, "typed run fills the find field",
          repr([l for l in txt.split("\n") if "find" in l]))


def find_head(txt):
    for l in txt.split("\n"):
        if l.startswith(" find: "):
            return l
    return ""


def case_find_options(exe, tmp):
    """Alt-C / Alt-W / Alt-R (ESC + letter) toggle ignore case, whole word
    and regex in the find field; the header lists them and the hit count
    follows. Alt-C must not copy, Alt-R must not type."""
    body = b"Alpha alpha alphabet\nALPHA a.pha\n"
    path = scratch_file(tmp, "opts.txt", body)
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    heads = []
    try:
        t.pump(0.8)
        t.send(b"\x06", 0.3)          # Ctrl-F
        t.send(b"alpha", 0.5)
        heads.append(find_head(screen_text(t) or ""))
        t.send(b"\x1bc", 0.5)         # Alt-C: ignore case
        heads.append(find_head(screen_text(t) or ""))
        t.send(b"\x1bw", 0.5)         # Alt-W: whole word
        heads.append(find_head(screen_text(t) or ""))
        t.send(b"\x1bw", 0.3)         # Alt-W off
        t.send(b"\x7f\x7f\x7f\x7f", 0.3)
        t.send(b".pha", 0.3)          # "a.pha"
        t.send(b"\x1br", 0.5)         # Alt-R: regex
        heads.append(find_head(screen_text(t) or ""))
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if pyte is None:
        print("skip: find options (no pyte)")
        return
    check("0/2" in heads[0] and "icase" not in heads[0], "plain alpha: 2 hits", repr(heads[0]))
    check("icase" in heads[1] and "0/4" in heads[1], "Alt-C: icase, 4 hits", repr(heads[1]))
    check("icase+word" in heads[2] and "0/3" in heads[2], "Alt-W: word, 3 hits",
          repr(heads[2]))
    check("icase+regex" in heads[3] and "0/5" in heads[3] and "a.pha" in heads[3],
          "Alt-R: regex a.pha, 5 hits, the field kept its text", repr(heads[3]))


def case_find_replace(exe, tmp):
    """Ctrl-F, the query, Ctrl-F again opens the replace row (focused);
    Enter selects the first hit, Enter again replaces it (and selects the
    next); Alt-A replaces the rest as one undo step; Ctrl-Z after closing
    find undoes only that group."""
    body = b"cat one\ncat two\ncat three\n"
    path = scratch_file(tmp, "repl.txt", body)
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    heads = []
    try:
        t.pump(0.8)
        t.send(b"\x06", 0.3)          # Ctrl-F
        t.send(b"cat", 0.4)
        t.send(b"\x06", 0.3)          # Ctrl-F again: replace row
        t.send(b"dog", 0.4)
        heads.append(screen_text(t) or "")
        t.send(b"\r", 0.3)            # Enter: select the first hit
        t.send(b"\r", 0.4)            # Enter: replace it
        t.send(b"\x1ba", 0.6)         # Alt-A: replace all
        heads.append(find_head(screen_text(t) or ""))
        t.send(b"\x1b", 0.4)          # Esc: close find
        t.send(b"\x1a", 0.4)          # Ctrl-Z: undo the replace-all group
        t.send(b"\x13", 0.4)          # Ctrl-S
        t.send(b"\x11", 0.2)          # Ctrl-Q
        t.wait_exit(5.0)
    finally:
        t.kill()
    with open(path, "rb") as f:
        got = f.read()
    check(got == b"dog one\ncat two\ncat three\n",
          "replace one, then replace all as one undo step", repr(got))
    if pyte is None:
        print("skip: find replace screen (no pyte)")
        return
    check(" repl: dog" in heads[0], "Ctrl-F again shows the replace row",
          repr([l for l in heads[0].split("\n") if "repl" in l]))
    check("2 replaced" in heads[1], "Alt-A: the header says N replaced", repr(heads[1]))


def case_esc_help(exe, tmp):
    path = scratch_file(tmp, "esc.txt", b"hello\n")
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        t.send(b"\x1b", 0.4)
        txt = screen_text(t)
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if txt is None:
        print("skip: esc help (no pyte)")
        return
    check("key bindings" in txt, "lone Esc opens help")


def esc_latency_ms(exe, tmp, name, env_extra=None, tries=5):
    """Best of `tries`: ms from a lone Esc to the help overlay's bytes."""
    path = scratch_file(tmp, name, b"hello\n")
    env = {"RTX_SAFE_HOME": os.path.join(tmp, "safe")}
    if env_extra:
        env.update(env_extra)
    t = Tui(exe, [path], env)
    lat = []
    try:
        t.pump(0.8)
        for _ in range(tries):
            n0 = len(t.out)
            t0 = time.time()
            os.write(t.fd, b"\x1b")
            while time.time() - t0 < 2.0 and b"key bindings" not in t.out[n0:]:
                t.pump(0.001)
            if b"key bindings" in t.out[n0:]:
                lat.append((time.time() - t0) * 1000)
            t.send(b"\x1b", 0.3)  # close help
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    return min(lat) if lat else None


def case_esc_fast(exe, tmp):
    """A lone Esc acts after ~10 ms (not the 40 ms a started sequence
    gets); settings.json esc_timeout_ms lengthens it, RTX_ESC_MS wins; a
    sequence whose tail comes 20 ms after ESC [ is still one key."""
    ms = esc_latency_ms(exe, tmp, "escf.txt")
    check(ms is not None and ms < 30, "lone Esc answers within 30 ms", repr(ms))
    home = config_home(tmp, "cfg_esc", settings='{"esc_timeout_ms": 150}')
    slow = esc_latency_ms(exe, tmp, "escs.txt", {"XDG_CONFIG_HOME": home}, tries=2)
    check(slow is not None and slow >= 140, "settings.json esc_timeout_ms 150", repr(slow))
    fast = esc_latency_ms(exe, tmp, "esce.txt",
                          {"XDG_CONFIG_HOME": home, "RTX_ESC_MS": "5"}, tries=3)
    check(fast is not None and fast < 30, "RTX_ESC_MS overrides settings.json", repr(fast))
    got, _ = edit_session(exe, tmp, [(b"\x1b[", 0.02), b"C", b"z"], body=b"ab",
                          name="escsplit.txt")
    check(got == b"azb", "ESC [ ... 20 ms ... C is Right, not text", repr(got))


def open_tui(exe, tmp, name, body=b"", args=(), env=None, settle=0.8,
             jobctl=False):
    path = scratch_file(tmp, name, body)
    e = {"RTX_SAFE_HOME": os.path.join(tmp, "safe")}
    if env:
        e.update(env)
    t = Tui(exe, list(args) + [path], e, jobctl=jobctl)
    t.pump(settle)
    return t, path


def tty_cooked(t):
    try:
        a = termios.tcgetattr(t.fd)
    except termios.error:
        return None
    return bool(a[3] & termios.ICANON) and bool(a[3] & termios.ECHO)


def case_stats_json_clean(exe, tmp):
    t, _ = open_tui(exe, tmp, "sj.txt", b"hello\n", args=["--stats-json"])
    try:
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    at = out.find(b"{")
    tail = out[at:] if at >= 0 else b""
    check(at >= 0 and b"\x1b" not in tail, "--stats-json output has no reset escapes after it",
          repr(tail[-40:]))
    check(out.count(b"\x1b[?1049l") == 1, "tty restore writes once",
          str(out.count(b"\x1b[?1049l")))


def case_sigterm_restores(exe, tmp):
    t, _ = open_tui(exe, tmp, "term.txt", b"hello\n")
    try:
        raw_before = tty_cooked(t)
        os.kill(t.pid, signal.SIGTERM)
        st = t.wait_exit(5.0)
        cooked = tty_cooked(t)
    finally:
        t.kill()
    out = bytes(t.out)
    check(raw_before is False, "editor runs raw")
    check(st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGTERM,
          "SIGTERM is re-raised", repr(st))
    check(out.rfind(b"\x1b[?1049l") > out.rfind(b"\x1b[?1049h"),
          "SIGTERM leaves the alt screen")
    check(cooked is True, "SIGTERM restores cooked mode", repr(cooked))


def case_sigsegv_restores(exe, tmp):
    t, _ = open_tui(exe, tmp, "segv.txt", b"hello\n")
    try:
        os.kill(t.pid, signal.SIGSEGV)
        st = t.wait_exit(5.0)
        cooked = tty_cooked(t)
    finally:
        t.kill()
    check(st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGSEGV,
          "SIGSEGV is re-raised", repr(st))
    check(cooked is True, "SIGSEGV restores cooked mode", repr(cooked))


def case_suspend_resume(exe, tmp):
    body = b"".join(b"row %d\n" % i for i in range(30))
    if not os.path.exists("/proc/self/task"):
        print("skip: suspend (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "tstp.txt", body, jobctl=True)
    try:
        os.kill(t.target, signal.SIGTSTP)
        t.pump(0.3)
        stopped = proc_state(t.target) == "T"
        cooked = tty_cooked(t)
        mark = len(t.out)
        os.kill(t.target, signal.SIGCONT)
        t.pump(0.6)
        after = bytes(t.out[mark:])
        raw_again = tty_cooked(t)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(stopped, "SIGTSTP stops the editor")
    check(cooked is True, "suspend hands back a cooked tty", repr(cooked))
    check(b"\x1b[?1049h" in after and b"\x1b[?2004h" in after,
          "resume re-enters alt screen and paste mode")
    check(raw_again is False, "resume is raw again", repr(raw_again))
    check(b"row 5" in after, "resume repaints", repr(after[-80:]))


def case_focus_repaint(exe, tmp):
    body = b"".join(b"row %d\n" % i for i in range(30))
    t, _ = open_tui(exe, tmp, "focus.txt", body)
    try:
        t.pump(0.5)
        mark = len(t.out)
        t.send(b"\x1b[I", 0.5)
        after = bytes(t.out[mark:])
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(b"row 5" in after and b"row 20" in after,
          "focus-in repaints every row", repr(after[:80]))


def screen_cells(t):
    """Every cell as (char, fg, bg, bold, reverse): what a frame left."""
    sc = t.screen()
    if sc is None:
        return None
    out = []
    for y in range(t.rows):
        line = sc.buffer[y]
        out.append([(line[x].data, line[x].fg, line[x].bg, line[x].bold,
                     line[x].reverse) for x in range(t.cols)])
    return out


def case_incremental_is_full(exe, tmp):
    """Frames after keys only rewrite rows that changed (row diff, the pair
    scan bounded to the visible rows). The screen they leave must equal a
    full repaint of the same state (focus-in clears and paints every row)."""
    fns = []
    for i in range(60):
        fns.append(b"static int f%d(int x) {\n"
                   b"    if (x > %d) { return (x * 2); } /* c %d */\n"
                   b"    return g(\"s%d\", x);\n}\n" % (i, i, i, i))
    env = {}
    g = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                     "testdata", "grammars"))
    if os.path.isdir(g):
        env["RTX_GRAMMARS"] = g
    t, _ = open_tui(exe, tmp, "inc.c", b"".join(fns), args=("--no-blink",),
                    env=env)
    bad = []
    try:
        # Into a brace block below the top, then type, delete, move, scroll.
        t.send(b"\x1b[B" * 9 + b"\x1b[C" * 12, 0.3)
        for step, keys in enumerate([b"Q", b"x", b"(", b"\x7f", b"}", b"\x1b[B",
                                     b"zz", b"\x1b[<65;10;10M", b"w",
                                     b"\x1b[<64;10;10M", b"\r", b"{"]):
            t.send(keys, 0.25)
            a = screen_cells(t)
            if a is None:
                break
            t.send(b"\x1b[I", 0.4)
            b = screen_cells(t)
            if a != b:
                rows = [y for y in range(t.rows) if a[y] != b[y]]
                bad.append((step, keys, rows[:4]))
        t.send(b"\x11", 0.3)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if pyte is None:
        print("skip: incremental is full (no pyte)")
        return
    check(not bad, "incremental frames equal a full repaint", repr(bad[:3]))


def case_scroll_regions_equal_full(exe, tmp):
    """Scrolls by 1, 3, a wheel notch (2), two notches and a page, both
    ways, in one pane (wrap off / on), two stacked panes, two side by
    side, with the find panel open and with the tab bar. Each frame moves
    rows with DECSTBM + DL / IL where it can; the screen it leaves (chars
    and attributes) must equal a full repaint (focus-in)."""
    import re
    if pyte is None:
        print("skip: scroll regions equal full (no pyte)")
        return
    region = re.compile(rb"\x1b\[\d+;\d+r")
    fns = []
    for i in range(80):
        fns.append(b"static int f%d(int x) {\n"
                   b"    if (x > %d) { return (x * 2); } /* c %d */\n"
                   b"    return g(\"s%d\", x);\n}\n" % (i, i, i, i))
    csrc = scratch_file(tmp, "sr.c", b"".join(fns))
    prose = scratch_file(tmp, "sr.txt", b"".join(
        b"%d " % i + b"lorem ipsum dolor sit amet " * (1 + i % 5) + b"\n"
        for i in range(200)))
    other = scratch_file(tmp, "sr2.txt", b"".join(b"other %d\n" % i for i in range(200)))
    env = {"RTX_SAFE_HOME": os.path.join(tmp, "safe_sr")}
    g = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                     "testdata", "grammars"))
    if os.path.isdir(g):
        env["RTX_GRAMMARS"] = g
    rows = 30
    down, up = b"\x1b[B", b"\x1b[A"

    def wheel(n, dn, y=10, x=10):
        return (b"\x1b[<%d;%d;%dM" % (65 if dn else 64, x, y)) * n

    one_pane = [down * (rows + 2), down, down * 3, wheel(1, True),
                wheel(2, True), b"\x1b[6~", b"\x1b[6~", up * (rows - 3), up,
                up * 3, wheel(1, False), wheel(2, False), b"\x1b[5~"]
    wheels = [wheel(1, True), wheel(2, True), wheel(3, True), wheel(1, False),
              wheel(2, False)]
    scenarios = [
        ("one pane", [csrc], [], b"", one_pane, True),
        ("wrap", ["--wrap", prose], [], b"", one_pane, True),
        ("stacked", [csrc], [], b"\x1f",
         wheels + [wheel(2, True, y=22), wheel(1, False, y=22), down * 3,
                   b"\x1b[6~"], True),
        ("side by side", [csrc], [], b"\x1c",
         wheels + [wheel(2, True, x=60), b"\x1b[6~"], False),
        ("find", [csrc], [], b"\x06f1", wheels, True),
        # Two files open side by side; Ctrl-] closes one pane and the
        # tab bar holds both buffers over one full-width pane.
        ("tabs", [csrc, other], [], b"\x1d", one_pane[:6], True),
    ]
    bad = []
    used = {}
    for name, files, args, setup, steps, want_region in scenarios:
        t = Tui(exe, ["--no-blink"] + list(args) + list(files), env, rows=rows,
                cols=100)
        try:
            t.pump(0.8)
            if setup:
                t.send(setup, 0.4)
            for step, keys in enumerate(steps):
                mark = len(t.out)
                t.send(keys, 0.3)
                if region.search(bytes(t.out[mark:])):
                    used[name] = used.get(name, 0) + 1
                a = screen_cells(t)
                t.send(b"\x1b[I", 0.3)
                b = screen_cells(t)
                if a != b:
                    diff = [y for y in range(t.rows) if a[y] != b[y]]
                    bad.append((name, step, keys[:12], diff[:4]))
            t.send(b"\x11", 0.3)
            t.send(b"q", 0.2)
            t.wait_exit(5.0)
        finally:
            t.kill()
        if want_region:
            check(used.get(name, 0) > 0, "scroll regions: %s moves rows" % name,
                  repr(used))
        else:
            check(used.get(name, 0) == 0,
                  "scroll regions: %s rewrites rows (no region)" % name, repr(used))
    check(not bad, "scroll regions: frames equal a full repaint", repr(bad[:4]))
    # Off switch: no DECSTBM at all.
    t = Tui(exe, ["--no-blink", "--no-scroll-regions", csrc], env, rows=rows,
            cols=100)
    try:
        t.pump(0.8)
        t.send(wheel(1, True), 0.3)
        t.send(b"\x1b[6~", 0.3)
        off = bytes(t.out)
        t.send(b"\x11", 0.3)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(not region.search(off), "scroll regions: --no-scroll-regions is off")


def case_clip_osc52(exe, tmp):
    t, _ = open_tui(exe, tmp, "clip.txt", b"hello world",
                    env={"SSH_TTY": "/dev/pts/99"})
    try:
        t.send(b"\x01", 0.3)   # select all
        t.send(b"\x03", 0.5)   # copy
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    check(b"\x1b]52;c;aGVsbG8gd29ybGQ=\x07" in out, "SSH copy uses OSC 52")


def case_clip_quiet(exe, tmp):
    # No DISPLAY / WAYLAND: xclip (if installed) fails on stderr; a missing
    # tool is 'not found'. Neither may print over the frame, and the copy
    # falls back to OSC 52.
    t, _ = open_tui(exe, tmp, "quiet.txt", b"abc")
    try:
        t.send(b"\x01", 0.3)
        t.send(b"\x03", 0.8)
        alive = os.waitpid(t.pid, os.WNOHANG)[0] == 0
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    check(alive, "copy without a clipboard tool keeps running")
    check(b"display" not in out.lower() and b"not found" not in out,
          "clipboard tool stderr is not painted")
    check(b"\x1b]52;c;YWJj\x07" in out, "failed tool falls back to OSC 52")


def proc_state(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        return "?"


def proc_cpu(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(")", 1)[1].split()
    return (int(parts[11]) + int(parts[12])) / os.sysconf("SC_CLK_TCK")


def case_stats_idle(exe, tmp):
    if not os.path.exists("/proc/self/stat"):
        print("skip: stats idle (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "stats.txt", b"hello\n")
    try:
        t.send(b"\x1b[27;5;61~", 0.5)  # Ctrl-= stats overlay
        c0 = proc_cpu(t.pid)
        t.pump(2.0)
        c1 = proc_cpu(t.pid)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(c1 - c0 < 0.6, "stats overlay does not spin a core",
          "%.2fs cpu in 2s" % (c1 - c0))


def proc_wakeups(pid):
    """Context switches over every thread: each is a sleep ending."""
    n = 0
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return 0
    for tid in tids:
        try:
            with open("/proc/%d/task/%s/status" % (pid, tid)) as f:
                for line in f:
                    if line.startswith(("voluntary_ctxt_switches",
                                        "nonvoluntary_ctxt_switches")):
                        n += int(line.split()[1])
        except OSError:
            pass
    return n


def thread_wakeups(pid):
    """{tid: context switches} for every thread of `pid`."""
    out = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return out
    for tid in tids:
        n = 0
        try:
            with open("/proc/%d/task/%s/status" % (pid, tid)) as f:
                for line in f:
                    if line.startswith(("voluntary_ctxt_switches",
                                        "nonvoluntary_ctxt_switches")):
                        n += int(line.split()[1])
        except OSError:
            continue
        out[tid] = n
    return out


def runtime_tick(pid, tid):
    """The ccc runtime's sysmon between its 20 ms ticks: a raw
    FUTEX_WAIT_PRIVATE (op 0x80) with a timeout, runtime/wake_primitive.h
    wait_timeout. Nothing cctext runs waits so (its condvars are
    FUTEX_WAIT_BITSET; the runtime's workers park with no timeout). A
    runtime whose sysmon sleeps while the scheduler is quiescent has no
    such thread at idle."""
    for _ in range(20):
        try:
            with open("/proc/%d/task/%s/syscall" % (pid, tid)) as f:
                v = f.read().split()
        except OSError:
            return False
        if v and v[0] == "running":
            time.sleep(0.001)
            continue
        return (len(v) > 4 and v[0] == "202" and int(v[2], 16) == 0x80 and
                int(v[4], 16) != 0)
    return False


# The pinned ccc (concurrent-c #162) sleeps its sysmon at idle: a tick is a
# failure. RTX_IDLE_STRICT=0 turns it back into a note (an older runtime).
IDLE_STRICT = os.environ.get("RTX_IDLE_STRICT", "1") not in ("", "0")


def idle_threads(pid, secs, pump):
    """Wakeups of every thread over `secs` once work has settled (a 0.5 s
    window where nothing but the runtime's tick wakes; 8 s at most):
    (own, tick, detail) — cctext's threads, the ccc runtime's sysmon, and
    'tid:comm:n' for each thread that woke."""
    def split(a, b):
        own = tick = 0
        detail = []
        for tid, n in b.items():
            d = n - a.get(tid, n)
            if d <= 0:
                continue
            try:
                with open("/proc/%d/task/%s/comm" % (pid, tid)) as f:
                    comm = f.read().strip()
            except OSError:
                comm = "?"
            if runtime_tick(pid, tid):
                tick += d
                detail.append("%s:ccc-sysmon:%d" % (tid, d))
            else:
                own += d
                detail.append("%s:%s:%d" % (tid, comm, d))
        return own, tick, " ".join(detail)
    end = time.time() + 8.0
    while time.time() < end:
        a = thread_wakeups(pid)
        pump(0.5)
        if split(a, thread_wakeups(pid))[0] == 0:
            break
    a = thread_wakeups(pid)
    pump(secs)
    return split(a, thread_wakeups(pid))


def check_idle(tag, pid, pump, secs=3.0):
    """Zero wakeups on every thread over `secs` of idle, the ccc runtime's
    sysmon included (it sleeps while the scheduler is quiescent since
    concurrent-c #162); RTX_IDLE_STRICT=0 makes its 20 ms tick a note,
    for an older runtime."""
    own, tick, detail = idle_threads(pid, secs, pump)
    check(own == 0, "%s: idle, no thread wakes" % tag,
          "%d wakeups in %.1f s: %s" % (own, secs, detail))
    if tick:
        if IDLE_STRICT:
            check(False, "%s: idle, the runtime's sysmon sleeps" % tag,
                  "%d ticks in %.1f s: %s" % (tick, secs, detail))
        else:
            print("note: %s: ccc runtime sysmon ticked %d times in %.1f s "
                  "(needs the quiescent-sysmon runtime)" % (tag, tick, secs))
    return own, tick


def quiet_window(t, secs):
    """Output bytes and wakeups while the editor is left alone."""
    mark = len(t.out)
    w0 = proc_wakeups(t.pid)
    t.pump(secs)
    return len(t.out) - mark, proc_wakeups(t.pid) - w0


CELL = ["--caret=cell"]


def case_blink_idle_stops(exe, tmp):
    """--caret=cell (the painted caret): it blinks after input, then stops
    (solid, no timer) once the idle timeout passes: an idle editor paints
    nothing and does not wake."""
    if not os.path.exists("/proc/self/stat"):
        print("skip: blink idle (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "blink.txt", b"hello\nworld\n", args=CELL,
                    env={"RTX_BLINK_IDLE_MS": "1000"})
    try:
        t.pump(1.5)  # launch counts as input: let that window run out
        t.send(b"\x1b[C", 0.05)  # Right: blinking again
        live, _ = quiet_window(t, 1.2)
        t.pump(0.8)  # past the 1 s idle timeout (+ one blink phase)
        idle, wake = quiet_window(t, 1.5)
        t.send(b"\x1b[D", 0.05)  # Left: blinks again
        again, _ = quiet_window(t, 1.2)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(live > 0, "cell caret blinks after input", "%d bytes" % live)
    check(idle == 0, "idle cell caret stops painting", "%d bytes" % idle)
    check(wake <= 2, "idle editor does not wake", "%d wakeups in 1.5s" % wake)
    check(again > 0, "input restarts the cell blink", "%d bytes" % again)


def case_blink_unfocused_stops(exe, tmp):
    """--caret=cell: focus-out (CSI O) stops the blink with the caret
    shown; focus-in (CSI I) repaints and blinks again."""
    if not os.path.exists("/proc/self/stat"):
        print("skip: blink unfocused (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "focus_blink.txt", b"hello\nworld\n", args=CELL)
    try:
        t.send(b"\x1b[O", 0.6)
        idle, wake = quiet_window(t, 1.5)
        t.send(b"\x1b[I", 0.3)
        mark = len(t.out)
        t.pump(1.2)
        back = len(t.out) - mark
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(idle == 0, "unfocused cell caret stops painting", "%d bytes" % idle)
    check(wake <= 2, "unfocused editor does not wake", "%d wakeups in 1.5s" % wake)
    check(back > 0, "focus-in blinks the cell again", "%d bytes" % back)


def case_no_blink_idle(exe, tmp):
    """--no-blink: nothing is due, so the editor sleeps until input; the
    terminal cursor is a steady bar."""
    if not os.path.exists("/proc/self/stat"):
        print("skip: no-blink idle (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "noblink.txt", b"hello\n", args=["--no-blink"])
    try:
        t.pump(0.6)
        idle, wake = quiet_window(t, 1.5)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    check(idle == 0, "--no-blink paints nothing idle", "%d bytes" % idle)
    check(wake <= 2, "--no-blink idle does not wake", "%d wakeups in 1.5s" % wake)
    check(b"\x1b[6 q" in out and b"\x1b[5 q" not in out,
          "--no-blink: steady bar cursor (DECSCUSR 6)")


def case_cursor_blinks_itself(exe, tmp):
    """Default: the terminal cursor is the caret and the terminal blinks it
    (DECSCUSR 5). The editor writes nothing and does not wake between
    keys: no blink timer at all, idle timeout or not."""
    if not os.path.exists("/proc/self/stat"):
        print("skip: cursor blink (no /proc)")
        return
    t, _ = open_tui(exe, tmp, "hwblink.txt", b"hello\nworld\n")
    try:
        t.send(b"\x1b[C", 0.3)
        live, wake = quiet_window(t, 2.0)
        mark = len(t.out)
        t.send(b"\x1b[O", 0.3)
        away = bytes(t.out[mark:])
        idle, wake_away = quiet_window(t, 1.0)
        mark = len(t.out)
        t.send(b"\x1b[I", 0.5)
        back = bytes(t.out[mark:])
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    check(b"\x1b[5 q" in out and b"\x1b[?25h" in out,
          "caret is the terminal cursor: blinking bar, shown")
    check(live == 0, "blinking caret: no bytes between keys", "%d bytes" % live)
    check(wake <= 2, "blinking caret: the editor does not wake",
          "%d wakeups in 2s" % wake)
    check(b"\x1b[6 q" in away and idle == 0 and wake_away <= 2,
          "focus-out: steady cursor, then nothing", repr(away[:40]))
    check(b"\x1b[5 q" in back, "focus-in: blinking again", repr(back[-40:]))


def case_cursor_soft_blink_vscode(exe, tmp):
    """Cursor / VS Code ignore DECSCUSR blink: the editor soft-blinks the
    host caret (steady bar + ?25l/?25h) on its own timer."""
    t, _ = open_tui(exe, tmp, "softblink.txt", b"hello\nworld\n",
                    env={"TERM_PROGRAM": "vscode"})
    try:
        mark = len(t.out)
        t.pump(1.8)
        idle = bytes(t.out[mark:])
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    warm = bytes(t.out[:mark])
    check(b"\x1b[6 q" in warm and b"\x1b[5 q" not in warm,
          "Cursor soft blink: steady bar shape",
          "warm 5=%d 6=%d" % (warm.count(b"\x1b[5 q"), warm.count(b"\x1b[6 q")))
    check(idle.count(b"\x1b[?25l") >= 1 and idle.count(b"\x1b[?25h") >= 1,
          "Cursor soft blink: hide/show on the timer",
          "25l=%d 25h=%d bytes=%d" % (idle.count(b"\x1b[?25l"),
                                       idle.count(b"\x1b[?25h"), len(idle)))


def cursor_at(t):
    """(x, y, hidden) of the terminal cursor as pyte replays the output."""
    sc = t.screen()
    if sc is None:
        return None
    return sc.cursor.x, sc.cursor.y, sc.cursor.hidden


def cell_char(t, x, y):
    sc = t.screen()
    if sc is None or y < 0 or y >= t.rows or x < 0 or x >= t.cols:
        return None
    return sc.buffer[y][x].data


def reverse_cells(t, rows):
    """Reverse-video cells on screen rows `rows` (the text area), less the
    scroll rails (a column of them)."""
    sc = t.screen()
    if sc is None:
        return None
    rev = [(x, y) for y in rows for x in range(t.cols)
           if sc.buffer[y][x].reverse]
    per = {}
    for x, _ in rev:
        per[x] = per.get(x, 0) + 1
    return [(x, y) for x, y in rev if per[x] <= 2]


def typed_left_of_cursor(t, ch, width=1):
    """The cursor sits just right of the glyph the user just typed."""
    c = cursor_at(t)
    if c is None:
        return False, None
    x, y, hidden = c
    left = cell_char(t, x - width, y)
    return (not hidden) and left == ch, (x, y, hidden, left)


def case_cursor_is_caret(exe, tmp):
    """The terminal cursor sits on the caret cell across typing, wide
    characters, a scrolled-off caret, and no reverse-video caret cell is
    painted (no double caret)."""
    if pyte is None:
        print("skip: cursor is caret (no pyte)")
        return
    body = b"".join(b"line %d\n" % i for i in range(80))
    t, _ = open_tui(exe, tmp, "hw.txt", body)
    res = {}
    try:
        t.send(b"\x1b[B\x1b[B\x1b[C\x1b[C", 0.3)  # row 3, col 2
        t.send(b"Q", 0.3)
        res["type"] = typed_left_of_cursor(t, "Q")
        res["rev"] = reverse_cells(t, range(t.rows - 1))
        t.send("日本".encode(), 0.3)  # two wide CJK glyphs
        res["wide"] = typed_left_of_cursor(t, "本", 2)
        t.send(b"\x1b[D", 0.3)  # back onto the second wide glyph
        c = cursor_at(t)
        res["on_wide"] = (bool(c) and cell_char(t, c[0], c[1]) == "本", c)
        t.send(b"\x1b[F", 0.3)  # End: past the last glyph
        c = cursor_at(t)
        res["end"] = (bool(c) and cell_char(t, c[0] - 1, c[1]) == "2" and
                      cell_char(t, c[0], c[1]) == " ", c)
        for _ in range(8):
            t.send(b"\x1b[<65;10;10M", 0.1)  # wheel: the caret scrolls off
        t.pump(0.3)
        c = cursor_at(t)
        res["off"] = (bool(c) and c[2], c)
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(res["type"][0], "cursor follows typing", repr(res["type"][1]))
    check(res["rev"] == [], "no reverse-video caret cell (no double caret)",
          repr(res["rev"][:4]))
    check(res["wide"][0], "cursor after a wide glyph", repr(res["wide"][1]))
    check(res["on_wide"][0], "cursor on a wide glyph", repr(res["on_wide"][1]))
    check(res["end"][0], "cursor at end of line", repr(res["end"][1]))
    check(res["off"][0], "caret scrolled off: cursor hidden", repr(res["off"][1]))


def case_cursor_rich(exe, tmp):
    """Rich pane: hidden hints (** markers) and an MD table. The cursor
    stays on the glyph the caret is at, not drifted by hidden bytes."""
    if pyte is None:
        print("skip: cursor rich (no pyte)")
        return
    body = (b"intro **bold** tail yz\n\n"
            b"| a | bb |\n|---|----|\n| c | dd |\n\nend\n")
    t, _ = open_tui(exe, tmp, "hw.md", body)
    res = {}
    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.4)  # Ctrl-D: Rich on
        t.send(b"\x1b[F\x1b[D", 0.3)  # End, Left: on the "z"
        c = cursor_at(t)
        line0 = (screen_text(t) or "").split("\n")[0]
        res["hint"] = (bool(c) and cell_char(t, c[0], c[1]) == "z", c, line0)
        t.send(b"\x1b[B\x1b[B\x1b[B\x1b[B\x1b[H", 0.3)  # the "| c | dd |" row
        t.send(b"Z", 0.4)
        res["table"] = typed_left_of_cursor(t, "Z")
        res["tabtxt"] = (screen_text(t) or "").split("\n")[:8]
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(res["hint"][0] and "**" not in res["hint"][2],
          "rich: cursor on the caret glyph past hidden hints",
          repr(res["hint"][1:]))
    check(res["table"][0], "rich MD table: cursor after the typed glyph",
          repr((res["table"][1], res["tabtxt"])))


def case_cursor_split_and_prompts(exe, tmp):
    """Split: the cursor is in the focused pane only. Find and jump fields
    own it while open; help hides it."""
    if pyte is None:
        print("skip: cursor split/prompts (no pyte)")
        return
    a = scratch_file(tmp, "left.txt", b"".join(b"left %d\n" % i for i in range(30)))
    b = scratch_file(tmp, "right.txt", b"".join(b"right %d\n" % i for i in range(30)))
    t = Tui(exe, [a, b], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    res = {}
    try:
        t.pump(0.8)
        t.send(b"R", 0.3)
        c1 = cursor_at(t)
        t.send(b"\x1b[17~", 0.3)  # F6: the other pane
        t.send(b"L", 0.3)
        c2 = cursor_at(t)
        res["split"] = (bool(c1 and c2) and not c1[2] and not c2[2] and
                        cell_char(t, c1[0] - 1, c1[1]) == "R" and
                        cell_char(t, c2[0] - 1, c2[1]) == "L" and
                        (c1[0] < t.cols // 2) != (c2[0] < t.cols // 2), c1, c2)
        res["rev"] = reverse_cells(t, range(t.rows - 1))
        t.send(b"\x06", 0.3)  # Ctrl-F
        t.send(b"left", 0.5)
        c = cursor_at(t)
        row = t.screen().display[c[1]] if c else ""
        res["find"] = (bool(c) and not c[2] and row.startswith(" find: left") and
                       c[0] == len(" find: left"), c, row[:20])
        t.send(b"\x1b", 0.3)
        t.send(b"\x07", 0.3)  # Ctrl-G: jump
        t.send(b"12", 0.3)
        c = cursor_at(t)
        row = t.screen().display[c[1]] if c else ""
        res["jump"] = (bool(c) and not c[2] and c[1] == t.rows - 1 and
                       row[c[0] - 2:c[0]] == "12", c, row[:30])
        t.send(b"\x1b", 0.3)
        t.send(b"\x1b", 0.4)  # lone Esc: help
        c = cursor_at(t)
        res["help"] = (bool(c) and c[2], c)
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(res["split"][0], "split: cursor in the focused pane",
          repr(res["split"][1:]))
    check(res["rev"] == [], "split: no painted caret in either pane",
          repr(res["rev"][:4]))
    check(res["find"][0], "find field owns the cursor", repr(res["find"][1:]))
    check(res["jump"][0], "jump field owns the cursor", repr(res["jump"][1:]))
    check(res["help"][0], "help overlay hides the cursor", repr(res["help"][1:]))


def case_cursor_hex(exe, tmp):
    """Hex: a block cursor on the nibble; the text column keeps a painted
    mirror cell."""
    if pyte is None:
        print("skip: cursor hex (no pyte)")
        return
    t, _ = open_tui(exe, tmp, "hw.bin", b"ABCDEFGH" * 8, args=["--hex"])
    try:
        t.send(b"\x1b[C\x1b[C", 0.3)
        c = cursor_at(t)
        ch = cell_char(t, c[0], c[1]) if c else None
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    # Byte 2 is "C" = 0x43: the cursor is on its high nibble "4".
    check(bool(c) and not c[2] and ch == "4", "hex: cursor on the caret's nibble",
          repr((c, ch)))
    check(b"\x1b[1 q" in out, "hex: block cursor (DECSCUSR 1)")


def case_cursor_grid(exe, tmp):
    """Grid (column) view: the cursor follows the caret across cells."""
    if pyte is None:
        print("skip: cursor grid (no pyte)")
        return
    t, _ = open_tui(exe, tmp, "hw.csv", b"name,qty\napple,3\npear,12\n",
                    args=["--grid"])
    try:
        t.send(b"\x1b[B\x1b[F", 0.3)  # row 2, End: after "3"
        t.send(b"Z", 0.4)
        res = typed_left_of_cursor(t, "Z")
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(res[0], "grid: cursor after the typed glyph", repr(res[1]))


def case_cursor_browse(exe, tmp):
    """Browse: the list has no caret; the cursor sits in the glob field,
    where typing goes."""
    if pyte is None:
        print("skip: cursor browse (no pyte)")
        return
    d = os.path.join(tmp, "brdir")
    os.makedirs(d, exist_ok=True)
    for n in ("alpha.txt", "beta.txt"):
        scratch_file(d, n, b"x\n")
    t = Tui(exe, [d], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        t.send(b"al", 0.4)
        c = cursor_at(t)
        row = t.screen().display[c[1]] if c else ""
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(bool(c) and not c[2] and row[:c[0]].endswith("glob: al"),
          "browse: cursor in the glob field", repr((c, row[:24])))


def case_browse_crumb(exe, tmp):
    """Browse: a click on a segment of the header path goes to that folder."""
    if pyte is None:
        print("skip: browse crumb (no pyte)")
        return
    d = os.path.realpath(os.path.join(tmp, "crumbs"))
    sub = os.path.join(d, "sub")
    os.makedirs(sub, exist_ok=True)
    scratch_file(sub, "x.txt", b"x\n")
    t = Tui(exe, [sub], {"RTX_SAFE_HOME": os.path.join(tmp, "safe")},
            cols=max(COLS, len(sub) + 8))
    try:
        t.pump(0.8)
        rows = t.screen().display
        y = next((i for i, r in enumerate(rows) if r.strip().endswith("/crumbs/sub")), -1)
        x = rows[y].find("/crumbs/") + 2 if y >= 0 else -1
        if y >= 0:
            t.send(b"\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (x + 1, y + 1, x + 1, y + 1), 0.6)
        head = t.screen().display[y].strip() if y >= 0 else ""
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(y >= 0 and head == d, "browse: clicking a path segment opens that folder",
          repr((y, head)))


def case_caret_cell_fallback(exe, tmp):
    """--caret=cell: the painted reverse-video caret, the cursor hidden."""
    if pyte is None:
        print("skip: caret cell (no pyte)")
        return
    t, _ = open_tui(exe, tmp, "cell.txt", b"hello\nworld\n",
                    args=CELL + ["--no-blink"])
    try:
        t.send(b"\x1b[C", 0.3)
        c = cursor_at(t)
        rev = reverse_cells(t, range(t.rows - 1))
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(bool(c) and c[2], "--caret=cell: terminal cursor hidden", repr(c))
    check(len(rev) == 1 and rev[0][1] == 0 and cell_char(t, rev[0][0], 0) == "e",
          "--caret=cell: one reverse-video caret cell", repr(rev))


def case_cursor_restored(exe, tmp):
    """Exit (and a fatal signal) reset the cursor shape and show it."""
    t, _ = open_tui(exe, tmp, "rest.txt", b"hello\n")
    try:
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    out = bytes(t.out)
    tail = out[out.rfind(b"\x1b[5 q"):]
    check(b"\x1b[0 q" in tail and tail.rfind(b"\x1b[?25h") > tail.find(b"\x1b[0 q"),
          "exit resets the cursor shape and shows it", repr(tail[-60:]))
    t, _ = open_tui(exe, tmp, "rest2.txt", b"hello\n")
    try:
        mark = len(t.out)
        os.kill(t.pid, signal.SIGTERM)
        t.wait_exit(5.0)
    finally:
        t.kill()
    after = bytes(t.out[mark:])
    check(b"\x1b[0 q" in after and b"\x1b[?25h" in after,
          "SIGTERM resets the cursor shape and shows it", repr(after[-60:]))


# Markdown editing (docs/md_view.md, Editing). Ctrl-End is ESC [1;5F.
CEND = b"\x1b[1;5F"
TBL = b"| A | B |\n|---|---|\n| x | y |\n"


def case_md_enter_continues(exe, tmp):
    got, _ = edit_session(exe, tmp, [CEND, b"\r", b"b"], body=b"- a",
                          name="enter.md")
    check(got == b"- a\n- b", "md: Enter continues a bullet", repr(got))
    got, _ = edit_session(exe, tmp, [b"\x1b[F", b"\r", b"z"],
                          body=b"1. a\n2. b\n", name="ordered.md")
    check(got == b"1. a\n2. z\n3. b\n", "md: Enter renumbers ordered siblings",
          repr(got))
    got, _ = edit_session(exe, tmp, [CEND, b"\r", b"c"], body=b"> - q",
                          name="quote.md")
    check(got == b"> - q\n> - c", "md: Enter continues a quoted item", repr(got))
    got, _ = edit_session(exe, tmp, [CEND, b"\x1b\r", b"c"], body=b"- ab",
                          name="soft.md")
    check(got == b"- ab\n  c", "md: Alt-Enter is a line inside the item",
          repr(got))


def case_md_enter_empty_exits(exe, tmp):
    got, _ = edit_session(exe, tmp, [CEND, b"\r", b"\r", b"x"], body=b"- a",
                          name="exit.md")
    check(got == b"- a\nx", "md: Enter on an empty item ends the list",
          repr(got))
    got, _ = edit_session(exe, tmp, [CEND, b"\r", b"\r", b"\x1a", b"\x1a"],
                          body=b"- a", name="exitundo.md")
    check(got == b"- a", "md: each Enter is one undo step", repr(got))


def case_md_tab_indent(exe, tmp):
    got, _ = edit_session(exe, tmp, [CEND, b"\t"], body=b"- a\n- b",
                          name="tab.md")
    check(got == b"- a\n  - b", "md: Tab nests a list item", repr(got))
    got, _ = edit_session(exe, tmp, [CEND, b"\x1b[Z"], body=b"- a\n  - b",
                          name="stab.md")
    check(got == b"- a\n- b", "md: Shift-Tab un-nests a list item", repr(got))
    got, _ = edit_session(exe, tmp, [CEND, b"\t"], body=b"1. a\n2. b\n3. c",
                          name="otab.md")
    check(got == b"1. a\n2. b\n   1. c", "md: Tab restarts ordered numbering",
          repr(got))


def case_md_autopair(exe, tmp):
    # One burst (a terminal paste without ?2004 or fast typing): the pair
    # policy still runs per scalar.
    got, _ = edit_session(exe, tmp, [b"see [x](u) `c` *e*"], name="pair.md")
    check(got == b"see [x](u) `c` *e*", "md: typed closers step over", repr(got))
    got, _ = edit_session(exe, tmp, [b"a [", b"\x7f", b"b"], name="pairbs.md")
    check(got == b"a b", "md: Backspace in an empty pair deletes both",
          repr(got))
    got, _ = edit_session(exe, tmp, [b"f(x) {"], name="pair.c")
    check(got == b"f(x) {}", "c: brackets pair", repr(got))
    got, _ = edit_session(exe, tmp, [b"f(x"], name="pair.txt")
    check(got == b"f(x", "txt: no pairs", repr(got))


def case_md_highlight(exe, tmp):
    """`==mark==`: Rich hides the markers and paints the text on the
    highlight background; Source shows the markers on it too; a shortcut
    reference `[ref]` with a definition is a link in Rich."""
    if pyte is None:
        print("skip: md highlight (no pyte)")
        return
    body = b"a ==mark== b [ref] c\n\n[ref]: https://x.io\n"
    t, _ = open_tui(exe, tmp, "hl.md", body)
    res = {}
    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.4)  # Ctrl-D: Rich on
        t.send(b"\x1b[B\x1b[B", 0.3)  # caret off the line: every mark hidden
        cells = screen_cells(t)
        line0 = (screen_text(t) or "").split("\n")[0]
        res["rich"] = (line0, cells[0][:14] if cells else None)
        t.send(b"\x04", 0.4)  # Source
        t.send(b"\x1b[A\x1b[A", 0.3)
        cells = screen_cells(t)
        lines = (screen_text(t) or "").split("\n")
        y = next((i for i, l in enumerate(lines) if "==mark" in l), 0)
        res["src"] = (lines[y] if lines else "", cells[y][:16] if cells else None)
        t.send(b"\x11", 0.2)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()

    def bg_at(row, text, needle, k=0):
        at = text.find(needle)
        if at < 0 or row is None or at + k >= len(row):
            return None
        return row[at + k][2]

    line, row = res["rich"]
    gut = len(line) - len(line.lstrip(" 0123456789"))
    body_txt = line[gut:] if gut else line
    check(body_txt.startswith("a mark b ref c"),
          "rich: ==mark== and [ref] markers hidden", repr(line))
    mark_bg = bg_at(row, line, "mark")
    a_bg = bg_at(row, line, "a mark")
    check(mark_bg not in (None, "default") and a_bg == "default",
          "rich: ==mark== on the highlight background", repr((mark_bg, a_bg)))
    line, row = res["src"]
    eq_bg = bg_at(row, line, "==mark")
    check("==mark==" in line and eq_bg not in (None, "default"),
          "source: == markers shown, styled with the mark", repr((line, eq_bg)))


def case_md_smart_paste(exe, tmp):
    left4 = b"\x1b[1;2D" * 4
    paste = b"\x1b[200~https://x.io\x1b[201~"
    got, _ = edit_session(exe, tmp, [CEND, left4, paste], body=b"see here",
                          name="paste.md")
    check(got == b"see [here](https://x.io)", "md: URL over a selection links",
          repr(got))
    got, _ = edit_session(exe, tmp, [CEND, left4, paste, b"\x1a"],
                          body=b"see here", name="pasteundo.md")
    check(got == b"see here", "md: link paste is one undo step", repr(got))


def case_md_table_col_delete(exe, tmp):
    # Source pane (Ctrl-D): the arrows step bytes, not Rich cells.
    to_x = [b"\x04", b"\x1b[B", b"\x1b[B", b"\x1b[C", b"\x1b[C"]
    got, _ = edit_session(exe, tmp, to_x + [b"\x1b.", b"x"], body=TBL,
                          name="tbl.md")
    check(got == b"| B |\n|---|\n| y |\n", "md: apply menu deletes a column",
          repr(got))
    got, _ = edit_session(exe, tmp, to_x + [b"\x1b.", b"r", b"Q"], body=TBL,
                          name="tblr.md")
    check(got == b"| A |  | B |\n|---|---|---|\n| x | Q | y |\n",
          "md: apply menu inserts a column", repr(got))
    got, _ = edit_session(exe, tmp, to_x + [b"\x1b.", b"x", b"\x1a"], body=TBL,
                          name="tblu.md")
    check(got == TBL, "md: column delete is one undo step", repr(got))


FIT_CELLS = [
    ("`palette.open`", "Opens the command palette from anywhere: every command "
     "with its **keys**, fuzzy matched as you type, and runs the one you pick"),
    ("`workspace.search_next_result_in_project`", "Steps to the next hit of "
     "the project search and opens its file at that line"),
    ("`go.line`", "short"),
]


def fit_expected(s):
    """Cell text as Rich paints it: backticks and ** hidden."""
    return s.replace("`", "").replace("**", "")


def fit_records(t):
    """Table records on screen: [[col0 text, col1 text], ...]. A record
    starts on the screen row with a gutter number; its wrap lines follow
    (rails `│`, gutter blank). Also returns the table's screen rows."""
    recs, rows = [], []
    txt = (screen_text(t) or "").split("\n")
    for line in txt[:-1]:
        i = line.find("│")
        if i < 0 or "├" in line:
            continue
        rows.append(line)
        gutter, body = line[:i], line[i:]
        parts = body.split("│")[1:-1]
        if gutter.strip():
            recs.append([[] for _ in parts])
        if not recs:
            continue
        for c, p in enumerate(parts[:len(recs[-1])]):
            if p.strip():
                recs[-1][c].append(p.strip())
    return recs, rows


def case_md_table_fit(exe, tmp):
    """An 80-column terminal, Rich, a 2-column table with long cells, soft
    wrap off (Alt-M) and on (the default): the table fits the pane — every
    table row ends with its right rail inside 80 columns — the cells wrap,
    and all the cell text is on screen (hidden marks drop out)."""
    if pyte is None:
        print("skip: md table fit (no pyte)")
        return
    body = b"# Fit\n\n| command | id |\n|---|---|\n"
    for a, b in FIT_CELLS:
        body += ("| %s | %s |\n" % (a, b)).encode()
    body += b"\nafter the table\n"
    for wrap, tag in ((0, "wrap off"), (1, "wrap on")):
        t, _ = open_tui(exe, tmp, "fit_%d.md" % wrap, body)
        try:
            txt = screen_text(t) or ""
            status = txt.split("\n")[-1]
            if "rich" not in status:
                t.send(b"\x04", 0.4)  # Ctrl-D: Rich on
            if (" unwr" in status) == bool(wrap):
                t.send(b"\x1bm", 0.4)  # Alt-M: wrap on / off
            status = (screen_text(t) or "").split("\n")[-1]
            recs, rows = fit_records(t)
            t.send(b"\x11", 0.2)
            t.send(b"q", 0.2)
            t.wait_exit(5.0)
        finally:
            t.kill()
        check((" unwr" in status) != bool(wrap),
              "md table fit (%s): the pane is in that mode" % tag, status)
        check(len(recs) == 1 + len(FIT_CELLS),
              "md table fit (%s): header and every record on screen" % tag,
              repr(rows))
        ends = [r.rstrip() for r in rows]
        check(bool(ends) and all(e.endswith("│") and len(e) <= COLS for e in ends),
              "md table fit (%s): every table row fits 80 columns" % tag,
              repr(ends))
        check(any(len(r) > 1 and any(len(c) > 1 for c in r) for r in recs),
              "md table fit (%s): long cells wrap" % tag, repr(recs))
        ok, why = True, ""
        for k, (a, b) in enumerate(FIT_CELLS):
            if k + 1 >= len(recs):
                ok, why = False, "record %d missing" % k
                break
            got0 = "".join(recs[k + 1][0])
            got1 = " ".join(recs[k + 1][1]) if len(recs[k + 1]) > 1 else ""
            if got0 != fit_expected(a):
                ok, why = False, "col 0 %r != %r" % (got0, fit_expected(a))
            want1 = fit_expected(b).split()
            if got1.split() != want1 and got1.replace(" ", "") != "".join(want1):
                ok, why = False, "col 1 %r != %r" % (got1, fit_expected(b))
        check(ok, "md table fit (%s): all cell text is on screen" % tag, why)


# Command palette and keymap (README: Keys and commands).
F1 = b"\x1bOP"
KITTY_CTRL_SHIFT_P = b"\x1b[112;6u"


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def config_home(tmp, name, keys=None, settings=None):
    """An XDG_CONFIG_HOME with cctext/keys.json and / or settings.json."""
    home = os.path.join(tmp, name)
    os.makedirs(os.path.join(home, "cctext"), exist_ok=True)
    if keys is not None:
        with open(os.path.join(home, "cctext", "keys.json"), "w") as f:
            f.write(keys)
    if settings is not None:
        with open(os.path.join(home, "cctext", "settings.json"), "w") as f:
            f.write(settings)
    return home


def palette_rows(txt):
    """The palette box: the query row and the command rows under the rule."""
    lines = txt.split("\n")
    for i, l in enumerate(lines):
        if " > " in l and i + 2 < len(lines) and "----" in lines[i + 1]:
            return l, [x for x in lines[i + 2:] if x.strip(" ~")]
    return "", []


def case_palette_run(exe, tmp):
    """F1 opens the palette, typing filters it, Enter runs the top row
    (Save writes the file); Esc closes it without running anything."""
    home = config_home(tmp, "cfg_pal")
    path = scratch_file(tmp, "pal.txt", b"")
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"),
                          "XDG_CONFIG_HOME": home})
    shots = {}
    try:
        t.pump(0.8)
        t.send(b"hello", 0.3)
        t.send(F1, 0.4)
        shots["open"] = screen_text(t)
        t.send(b"sav", 0.4)
        shots["filter"] = screen_text(t)
        t.send(b"\r", 0.5)
        saved = read_file(path)
        t.send(b"!", 0.3)
        t.send(F1, 0.3)
        t.send(b"\x1b", 0.4)          # Esc: close, run nothing
        shots["closed"] = screen_text(t)
        after_esc = read_file(path)
        t.send(b"\x11", 0.3)          # Ctrl-Q: unsaved "!" asks
        t.send(b"d", 0.3)              # discard
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(saved == b"hello", "palette: Enter on Save writes the file", repr(saved))
    check(after_esc == b"hello", "palette: Esc runs nothing", repr(after_esc))
    if pyte is None:
        print("skip: palette screen (no pyte)")
        return
    q, rows = palette_rows(shots["open"] or "")
    check(" commands" in (shots["open"] or "") and rows and "Command Palette" in rows[0],
          "palette: F1 opens it (table order, key hints)", repr(rows[:2]))
    q, rows = palette_rows(shots["filter"] or "")
    check("> sav" in q and rows and rows[0].strip(" ~").startswith("Save") and
          "Ctrl-S" in rows[0], "palette: typing filters to Save + its key", repr(rows[:2]))
    check(" commands" not in (shots["closed"] or ""), "palette: Esc closes it")


def case_palette_chords(exe, tmp):
    """Ctrl-Shift-P (kitty CSI u) and Esc then ':' open the palette; Quick
    Open lists with its Ctrl-P and Enter on it opens the picker; Enter on
    Wrap toggles wrap; the palette remembers it (recent first). (No row is
    reserved any more: cmd_table_smoke covers (unavailable).)"""
    home = config_home(tmp, "cfg_pal2")
    path = scratch_file(tmp, "pal2.txt", b"abc\n")
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"),
                          "XDG_CONFIG_HOME": home}, cols=200)
    shots = {}
    try:
        t.pump(0.8)
        t.send(KITTY_CTRL_SHIFT_P, 0.4)
        shots["kitty"] = screen_text(t)
        t.send(b"quick open", 0.4)
        shots["quick"] = screen_text(t)
        t.send(b"\r", 0.6)
        shots["unavail"] = screen_text(t)
        t.send(b"\x1b", 0.4)          # Esc: close the picker
        t.send(b"\x1b", 0.4)          # Esc: help
        t.send(b":", 0.4)              # ':' in help: the palette
        shots["esc_colon"] = screen_text(t)
        t.send(b"wrap on", 0.4)
        t.send(b"\r", 0.4)
        shots["ran"] = screen_text(t)
        t.send(F1, 0.4)
        shots["recent"] = screen_text(t)
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if pyte is None:
        print("skip: palette chords (no pyte)")
        return
    check(" commands" in (shots["kitty"] or ""), "palette: kitty Ctrl-Shift-P opens it")
    q, rows = palette_rows(shots["quick"] or "")
    check(rows and "Quick Open" in rows[0] and "Ctrl-P" in rows[0],
          "palette: Quick Open with its key", repr(rows[:2]))
    check(" open:" in (shots["unavail"] or "").split("\n")[0],
          "palette: Enter on it opens quick-open",
          repr((shots["unavail"] or "").split("\n")[0]))
    check(" commands" in (shots["esc_colon"] or ""), "palette: Esc : opens it")
    status = (shots["ran"] or "").split("\n")[-1]
    check("unwrap" in status or " wrap" in status, "palette: Enter runs Wrap", repr(status))
    q, rows = palette_rows(shots["recent"] or "")
    check(rows and rows[0].strip(" ~").startswith("Wrap"), "palette: recent first",
          repr(rows[:2]))


def case_keymap_file(exe, tmp):
    """keys.json under XDG_CONFIG_HOME: Ctrl-S unbound (does nothing),
    Ctrl-T bound to file.save."""
    home = config_home(tmp, "cfg_keys",
                       keys='{ "ctrl+s": null, // unbound\n "ctrl+t": "file.save" }')
    path = scratch_file(tmp, "km.txt", b"")
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"),
                          "XDG_CONFIG_HOME": home})
    try:
        t.pump(0.8)
        t.send(b"abc", 0.3)
        t.send(b"\x13", 0.4)          # Ctrl-S: unbound
        after_s = read_file(path)
        t.send(b"\x14", 0.5)          # Ctrl-T: save
        after_t = read_file(path)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(after_s == b"", "keys.json: null unbinds Ctrl-S", repr(after_s))
    check(after_t == b"abc", "keys.json: Ctrl-T runs file.save", repr(after_t))
    check(t.status is not None, "keys.json: quits after the save (not dirty)")


def case_keymap_flag(exe, tmp):
    """--keys FILE wins over the config dir; a broken keys file keeps the
    defaults and says so on the status bar."""
    kf = os.path.join(tmp, "k2.json")
    with open(kf, "w") as f:
        f.write('{"f5": "file.save"}')
    home = config_home(tmp, "cfg_keys2", keys='{"f5": "view.wrap"}')
    path = scratch_file(tmp, "kf.txt", b"")
    t = Tui(exe, ["--keys", kf, path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"),
                                        "XDG_CONFIG_HOME": home})
    try:
        t.pump(0.8)
        t.send(b"xyz", 0.3)
        t.send(b"\x1b[15~", 0.5)      # F5
        got = read_file(path)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(got == b"xyz", "--keys FILE: F5 saves", repr(got))
    bad = config_home(tmp, "cfg_bad", keys='{"ctrl+t": ')
    path = scratch_file(tmp, "kb.txt", b"")
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"),
                          "XDG_CONFIG_HOME": bad}, cols=200)
    try:
        t.pump(0.8)
        txt = screen_text(t)
        t.send(b"q", 0.2)
        t.send(b"\x13", 0.4)          # Ctrl-S still saves (defaults kept)
        got = read_file(path)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(got == b"q", "broken keys.json keeps the defaults", repr(got))
    if txt is not None:
        check("keys: JSON error" in txt.split("\n")[-1], "broken keys.json: status bar says so",
              repr(txt.split("\n")[-1]))


def case_settings_file(exe, tmp):
    """settings.json supplies defaults (tab_spaces); a flag still wins."""
    home = config_home(tmp, "cfg_set", settings='{"tab_spaces": 2, "autopair": false}')
    path = scratch_file(tmp, "st.txt", b"")
    env = {"RTX_SAFE_HOME": os.path.join(tmp, "safe"), "XDG_CONFIG_HOME": home}
    t = Tui(exe, [path], env)
    try:
        t.pump(0.8)
        t.send(b"\t(x", 0.4)
        t.send(b"\x13", 0.4)
        got = read_file(path)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(got == b"  (x", "settings.json: tab_spaces 2, autopair off", repr(got))
    path = scratch_file(tmp, "st2.txt", b"")
    t = Tui(exe, ["--tab-spaces=4", path], env)
    try:
        t.pump(0.8)
        t.send(b"\t", 0.4)
        t.send(b"\x13", 0.4)
        got = read_file(path)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(got == b"    ", "--tab-spaces overrides settings.json", repr(got))


def case_tabs_and_panes(exe, tmp):
    """Tab bar, Alt-W close with the unsaved prompt, three panes, F6
    focus cycling, Ctrl-] close pane, and the last close keeps running."""
    if pyte is None:
        print("skip: tabs and panes (no pyte)")
        return
    a = scratch_file(tmp, "tab_a.txt", b"".join(b"aaa %d\n" % i for i in range(40)))
    b = scratch_file(tmp, "tab_b.txt", b"".join(b"bbb %d\n" % i for i in range(40)))
    c = scratch_file(tmp, "tab_c.txt", b"".join(b"ccc %d\n" % i for i in range(40)))
    t = Tui(exe, [a, b, c], {"RTX_SAFE_HOME": os.path.join(tmp, "safe_tabs")})
    res = {}

    def disp():
        sc = t.screen()
        return sc.display if sc else []

    def status():
        d = disp()
        return d[-1] if d else ""

    def focus_path():
        s = status()
        return s.split()[0].lstrip("*") if s.split() else ""

    try:
        t.pump(0.8)
        d = disp()
        res["bar"] = (all(n in d[0] for n in ("tab_a.txt", "tab_b.txt", "tab_c.txt")),
                      d[0])
        t.send(b"Z", 0.3)  # dirty the focused tab_a
        res["dot"] = ("●" in disp()[0], disp()[0])
        t.send(b"\x1bw", 0.4)  # Alt-W (buffer.close): asks
        res["ask"] = ("close tab_a.txt" in status() and "don't save" in status(),
                      status())
        t.send(b"q", 0.5)  # don't save
        d = disp()
        res["closed"] = ("tab_a.txt" not in d[0] and "tab_b.txt" in d[0] and
                         "bbb 0" in d[1], d[0], d[1])
        t.send(b"\x1f", 0.4)  # Ctrl-_: split down -> three panes
        d = disp()
        res["three"] = (any("───" in row for row in d[1:-1]) and
                        sum(1 for row in d[1:-1] if "│" in row) > 5, d[:6])
        c0 = cursor_at(t)
        seen = []
        for _ in range(3):
            t.send(b"\x1b[17~", 0.3)  # F6
            c1 = cursor_at(t)
            seen.append((focus_path(), c1[:2] if c1 else None))
        res["cycle"] = (len(set(p for _, p in seen)) == 3 and
                        c0 is not None and seen[-1][1] == c0[:2], seen)
        t.send(b"\x1d", 0.4)  # Ctrl-]: close the focused pane
        d = disp()
        res["pane_closed"] = (not any("───" in row for row in d[1:-1]), d[:4])
        # Close everything: the editor keeps running on an untitled buffer.
        t.send(b"\x1bw", 0.4)
        t.send(b"\x1bw", 0.4)
        t.send(b"\x1bw", 0.4)
        res["alive"] = (t.pid is not None and "untitled" in status(), status())
        t.send(b"\x11", 0.3)
        t.send(b"q", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check(res["bar"][0], "tabs: the bar lists every buffer", repr(res["bar"][1]))
    check(res["dot"][0], "tabs: unsaved dot", repr(res["dot"][1]))
    check(res["ask"][0], "tabs: buffer.close on unsaved asks", repr(res["ask"][1]))
    check(res["closed"][0], "tabs: don't-save closes, the pane shows the next tab",
          repr(res["closed"][1:]))
    check(res["three"][0], "panes: split down makes three panes", repr(res["three"][1]))
    check(res["cycle"][0], "panes: F6 cycles focus through three panes",
          repr(res["cycle"][1]))
    check(res["pane_closed"][0], "panes: Ctrl-] closes a pane",
          repr(res["pane_closed"][1]))
    check(res["alive"][0], "tabs: closing the last buffer does not quit",
          repr(res["alive"][1]))


def proj_tree(tmp):
    root = os.path.join(tmp, "proj")
    for d in ("", ".git", "src", "src/deep/er", "docs"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    files = {
        "notes.md": b"# notes\n",
        "src/alpha_widget.c": b"int alpha;\n",
        "src/beta.c": b"line one\nline two\n    call(needle_x); /* here */\nend\n",
        "src/deep/er/gamma.h": b"#define G 1\n",
        "docs/widget_guide.md": b"guide\n",
        "ignored.log": b"needle_x in a log\n",
        ".gitignore": b"*.log\n",
    }
    for rel, body in files.items():
        with open(os.path.join(root, rel), "wb") as f:
            f.write(body)
    return root


def status_row(t):
    sc = t.screen()
    if sc is None:
        return None
    return sc.display[t.rows - 1]


def case_quick_open(exe, tmp):
    """Ctrl-P, a fuzzy name, Enter: the file opens in the focused pane."""
    root = proj_tree(tmp)
    t = Tui(exe, [os.path.join(root, "notes.md")],
            {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        t.send(b"\x10", 0.5)            # Ctrl-P
        t.send(b"alwid", 0.6)
        txt = screen_text(t)
        t.send(b"\r", 0.6)
        st = status_row(t)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if txt is None:
        print("skip: quick-open (no pyte)")
        return
    check(" open: alwid" in txt, "quick-open field shows the query",
          repr(txt.split("\n")[:3]))
    lines = txt.split("\n")
    check(len(lines) > 2 and "src/alpha_widget.c" in lines[2],
          "quick-open ranks the fuzzy match first", repr(lines[:4]))
    check(st is not None and "alpha_widget.c" in st,
          "Enter opens the file", repr(st))


def case_project_search(exe, tmp):
    """Alt-Shift-F (Ctrl-Shift-F), a query, Enter on the hit: the file
    opens with the match selected on screen; results are grouped."""
    root = proj_tree(tmp)
    t = Tui(exe, [os.path.join(root, "notes.md")],
            {"RTX_SAFE_HOME": os.path.join(tmp, "safe")})
    try:
        t.pump(0.8)
        t.send(b"\x1b[102;6u", 0.5)     # Ctrl-Shift-F (CSI u)
        t.send(b"needle_x", 0.8)        # debounce, then the search
        txt = screen_text(t)
        t.send(b"\x1b[B", 0.3)          # down: the hit row under its file
        t.send(b"\r", 0.8)
        st = status_row(t)
        after = screen_text(t)
        cur = cursor_at(t)
        t.send(b"\x1bF", 0.5)           # Alt-Shift-F: results kept
        again = screen_text(t)
        t.send(b"\x1b", 0.3)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if txt is None:
        print("skip: project search (no pyte)")
        return
    lines = txt.split("\n")
    check(" search: needle_x" in txt, "search field shows the query", repr(lines[:3]))
    check("1 matches in 1 files" in lines[1], "search header counts (gitignored log skipped)",
          repr(lines[1]))
    check("src/beta.c" in lines[2] and "(1)" in lines[2], "group row: file and count",
          repr(lines[2]))
    check("3" in lines[3] and "call(needle_x)" in lines[3], "hit row: line and preview",
          repr(lines[3]))
    check(st is not None and "beta.c" in st, "Enter opens the hit's file", repr(st))
    check(after is not None and "call(needle_x)" in after, "hit line is on screen")
    check(cur is not None and cur[1] >= 0, "cursor placed", repr(cur))
    check(again is not None and "src/beta.c" in again.split("\n")[2],
          "results survive closing the overlay")


DEMO_DECK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "testdata", "slides", "demo.md")


def case_present(exe, tmp):
    """Marp presentation mode (docs/slides.md): Shift-F5 shows the slide in
    a box, build steps appear one per key, digits + Enter go to a slide,
    a transition paints and then stops (no idle output), Esc comes back
    with the caret on the slide; the jump field takes #N."""
    with open(DEMO_DECK, "rb") as f:
        body = f.read()
    t, _ = open_tui(exe, tmp, "deck.md", body)
    shots = {}
    try:
        shots["edit"] = status_row(t)
        t.send(b"\x1b[15;2~", 0.6)            # Shift-F5
        shots["title"] = screen_text(t)
        t.send(b" ", 0.5)                      # slide 2, step 0
        shots["s2"] = screen_text(t)
        t.send(b" ", 0.5)                      # step 1
        shots["s2a"] = screen_text(t)
        t.send(b"\x1b[D", 0.5)                 # Left: step 0
        shots["s2b"] = screen_text(t)
        t.send(b"3\r", 0.8)                    # slide 3
        t.send(b"\x1b[6~", 0.05)               # PgDn: 3 -> step 1 (fragment)
        t.send(b"\x1b[6~\x1b[6~\x1b[6~", 0.6)   # steps 2, 3 then slide 4 (push)
        shots["s4"] = screen_text(t)
        quiet_bytes, quiet_wake = quiet_window(t, 1.2)
        t.send(b"\x1b[F", 0.6)                 # End
        shots["end"] = screen_text(t)
        t.send(b"\x1b[H", 0.6)                 # Home
        shots["home"] = screen_text(t)
        t.send(b"4\r", 0.8)
        t.send(b"\x1b", 0.6)                   # Esc: back to the editor
        shots["back"] = status_row(t)
        t.send(b"\x07", 0.3)                   # Ctrl-G, then #6
        t.send(b"#6", 0.3)
        shots["jump"] = status_row(t)
        t.send(b"\r", 0.5)
        shots["jumped"] = status_row(t)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    if shots["title"] is None:
        print("skip: present (no pyte)")
        return
    check("slide 1/11" in (shots["edit"] or ""), "present: status shows slide 1/11",
          repr(shots["edit"]))
    title = shots["title"].split("\n")
    check(title[0].lstrip().startswith("\u250c") and "cctext presents" in shots["title"],
          "present: Shift-F5 draws the title slide in a box", repr(title[:3]))
    check("slide 1/11" in title[-1], "present: bottom row names the slide", repr(title[-1]))
    check("Build steps" in shots["s2"] and "First, the bytes" not in shots["s2"],
          "present: slide 2 starts with its steps hidden")
    check("First, the bytes" in shots["s2a"] and "Then the block pass" not in shots["s2a"],
          "present: one key shows one step")
    check("First, the bytes" not in shots["s2b"], "present: Left hides the step again")
    check("Code keeps its colours" in shots["s4"] and "int main(void)" in shots["s4"],
          "present: steps then the push to slide 4", repr(shots["s4"].split("\n")[:5]))
    check(quiet_bytes == 0 and quiet_wake <= 2,
          "present: a still slide writes nothing and does not wake",
          "%d bytes, %d wakeups" % (quiet_bytes, quiet_wake))
    check("Thanks" in shots["end"], "present: End goes to the last slide")
    check("cctext presents" in shots["home"], "present: Home goes to the first slide")
    check("slide 4/11" in (shots["back"] or ""), "present: Esc comes back on slide 4",
          repr(shots["back"]))
    check("go to slide: #6" in (shots["jump"] or ""), "go.slide: the jump field takes #N",
          repr(shots["jump"]))
    check("slide 6/11" in (shots["jumped"] or ""), "go.slide: #6 Enter lands on slide 6",
          repr(shots["jumped"]))
    import subprocess
    out = subprocess.run([exe, "--batch", DEMO_DECK, "-c", "slides"], capture_output=True,
                         text=True, env=dict(os.environ, RTX_SAFE_HOME=os.path.join(tmp, "safe")))
    rows = [l.split("\t") for l in out.stdout.splitlines() if not l.startswith("#")]
    check(len(rows) == 11 and rows[1][2] == "4" and rows[1][4] == "Build steps" and
          rows[9][3] == "morph", "batch slides: one outline row per slide",
          repr(out.stdout[:200] + out.stderr[:200]))


def case_wb_annotation(exe, tmp):
    """Workbook (docs/workbook.md "Live value"): the caret in a formula
    shows ` → value` after it, dimmed, changing with every keystroke (an
    error while half-typed), in Rich and Source; the cursor stays at the
    formula's end, before the annotation; the file never holds it."""
    if pyte is None:
        print("skip: wb annotation (no pyte)")
        return
    body = (b"# S\n\nTable: T\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | =@a * 2 |\n\n"
            b"```calc\ns = sum(T.a)\n```\n")
    t, path = open_tui(exe, tmp, "w.wb.md", body)
    res = {}

    def calc_line():
        for ln in (screen_text(t) or "").split("\n"):
            if "s = " in ln:
                return ln
        return ""

    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.4)  # Ctrl-D: Rich on
        res["before"] = calc_line()
        t.send(b"\x1b[1;5F", 0.3)  # Ctrl-End
        t.send(b"\x1b[A\x1b[A\x1b[F", 0.4)  # Up, Up, End: after `)`
        res["on"] = calc_line()
        res["dim"] = None
        sc = t.screen()
        for y in range(t.rows):
            row = "".join(sc.buffer[y][x].data for x in range(t.cols))
            if "s = sum" in row and "\u2192" in row:
                x = row.index("\u2192")
                res["dim"] = (sc.buffer[y][x].fg, sc.buffer[y][row.index("s = ")].fg)
        c = cursor_at(t)
        res["cur"] = (c, cell_char(t, c[0] - 1, c[1]) if c else None)
        steps = []
        for key, want in ((b"+", "#parse"), (b"1", "5"), (b"0", "14")):
            t.send(key, 0.35)
            steps.append((key, want, calc_line()))
        res["steps"] = steps
        t.send(b"\x7f" * 4, 0.5)  # back to `s = sum(T.a`: half-typed
        res["half"] = calc_line()
        t.send(b"\x04", 0.4)  # Ctrl-D: Source
        res["source"] = calc_line()
        t.send(b")", 0.3)
        t.send(b"\x1b[A", 0.4)  # the caret leaves the formula
        res["off"] = calc_line()
        t.send(b"\x04", 0.4)  # Ctrl-D: Rich again
        # the table cell `=@a * 2`: from the end, up to its row; End, Left
        # past `|` and ` `
        t.send(b"\x1b[1;5F", 0.3)
        t.send(b"\x1b[A\x1b[A\x1b[A\x1b[A\x1b[F\x1b[D\x1b[D", 0.5)
        res["cell"] = [ln for ln in (screen_text(t) or "").split("\n") if "=@a" in ln]
        c = cursor_at(t)
        res["cellcur"] = (c, cell_char(t, c[0] - 1, c[1]) if c else None)
        t.send(b"\x13", 0.4)  # Ctrl-S
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    with open(path, "rb") as f:
        disk = f.read()
    arrow = "→"
    check(arrow not in res["before"] and "s = 4" in res["before"],
          "wb: a calc line paints its value", repr(res["before"]))
    check(("s = sum(T.a) " + arrow + " 4 ") in res["on"],
          "wb: caret in the formula: the formula, then its value", repr(res["on"]))
    check(bool(res["dim"]) and res["dim"][0] != res["dim"][1],
          "wb: the annotation paints in its own (dim) colour", repr(res["dim"]))
    check(bool(res["cur"][0]) and res["cur"][1] == ")",
          "wb: the cursor sits at the formula's end, before the annotation",
          repr(res["cur"]))
    for key, want, line in res["steps"]:
        check((arrow + " " + want) in line,
              "wb: keystroke %r: annotation %s" % (key, want), repr(line))
    check((arrow + " #parse(") in res["half"],
          "wb: a half-typed formula shows its error", repr(res["half"]))
    check(("s = sum(T.a " + arrow + " #parse(") in res["source"],
          "wb: Source view shows the annotation too", repr(res["source"]))
    check(arrow not in res["off"], "wb: the caret leaves: no annotation", repr(res["off"]))
    check(bool(res["cell"]) and ("=@a * 2 " + arrow + " 6") in res["cell"][0],
          "wb: a table cell formula shows its value after it", repr(res["cell"]))
    check(bool(res["cellcur"][0]) and res["cellcur"][1] == "2",
          "wb: in the cell, the cursor sits before the annotation", repr(res["cellcur"]))
    check(arrow.encode() not in disk and b"s = sum(T.a)\n" in disk,
          "wb: the file never holds the annotation", repr(disk[-40:]))


def case_wb_deferred(exe, tmp):
    """Workbook at scale (docs/workbook.md "W1: the lazy engine"): in a
    workbook over RTX_WB_DEFER_BYTES (1 MiB) a structural edit does not
    reparse on the keystroke; the read is a job the idle loop steps in
    slices, values hide while it reads, the status says recalculating,
    and the values paint again when it ends."""
    if pyte is None:
        print("skip: wb deferred (no pyte)")
        return
    rows = b"".join(b"| %d | =@a * 2 |\n" % i for i in range(1, 400001))
    body = (b"# D\n\n```calc\ns = sum(T.a)\n```\n\nTable: T\n\n| a | b |\n|---|---|\n" +
            rows)
    t, path = open_tui(exe, tmp, "big.wb.md", body, settle=4.0)
    res = {}

    def calc_line():
        for ln in (screen_text(t) or "").split("\n"):
            if "s = " in ln:
                return ln
        return ""

    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.6)  # Ctrl-D: Rich on
        res["before"] = calc_line()
        t.send(b"\x1b[1;5H", 0.3)  # Ctrl-Home
        t.send(b"\x1b[B\x1b[B\x1b[F", 0.3)  # the ```calc line, its end
        # break the fence and mend it in one burst: a structural edit
        t.send(b"x\x7f", 0.05)
        txt = screen_text(t) or ""
        res["mid"] = calc_line()
        res["mid_status"] = txt.split("\n")[-1]
        t.pump(4.0)
        res["after"] = calc_line()
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check("s = " in res["before"] and "sum(" not in res["before"],
          "wb big: a calc line paints its value", repr(res["before"]))
    check("sum(" in res["mid"],
          "wb big: a structural edit hides the values (no reparse on the keystroke)",
          repr(res["mid"]))
    check("recalculating" in res["mid_status"],
          "wb big: the status says the reparse waits", repr(res["mid_status"]))
    check("s = " in res["after"] and "sum(" not in res["after"],
          "wb big: the idle loop reparses and repaints", repr(res["after"]))


def _line_with(t, needle):
    for ln in (screen_text(t) or "").split("\n"):
        if needle in ln:
            return ln
    return ""


def _fg_of(t, needle, sub):
    """(fg of `sub`'s first char, fg of `needle`'s first char) on the row
    holding `needle`, else None."""
    sc = t.screen()
    for y in range(t.rows):
        row = "".join(sc.buffer[y][x].data for x in range(t.cols))
        if needle in row and sub in row[row.index(needle):]:
            x0 = row.index(needle)
            x1 = row.index(sub, x0)
            return (sc.buffer[y][x1].fg, sc.buffer[y][x0].fg)
    return None


def case_wb_stale(exe, tmp):
    """Workbook W1 stale display (docs/workbook.md "W1: the lazy engine"):
    a param flip over a big table is a job; until it ends the value paints
    its last value after `≈`, dimmed (its own theme colour), and the status
    says recalculating; Esc cancels (the stale mark stays, the status says
    so); the next edit plans the job again and the value paints fresh."""
    if pyte is None:
        print("skip: wb stale (no pyte)")
        return
    rows = b"".join(b"| %d |\n" % (i % 10) for i in range(300000))
    body = (b"# S\n\n```params\nlim = 5\n```\n\n```calc\nbig = sum(T.x where x > lim)\n```\n\n"
            b"Table: T\n\n| x |\n|---|\n" + rows)
    env = {"RTX_WB_INLINE_WORK": "512", "RTX_WB_SLICE_MS": "1", "RTX_WB_SLICE_ROWS": "64",
           "RTX_WB_SLICE_PAUSE_MS": "2"}  # the job outlasts the keys below
    path = scratch_file(tmp, "stale.wb.md", body)
    env["RTX_SAFE_HOME"] = os.path.join(tmp, "safe")
    t = Tui(exe, [path], env, cols=200)  # wide: the status after the key hints
    t.pump(6.0)
    res = {}
    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.5)  # Ctrl-D: Rich on
        t.pump(1.5)
        res["before"] = _line_with(t, "big = ")
        t.send(b"\x1b[1;5H", 0.3)  # Ctrl-Home
        t.send(b"\x1b[B\x1b[B\x1b[B\x1b[F", 0.3)  # `lim = 5`, its end
        t.send(b"\x1b[1;2D", 0.2)  # Shift-Left: select the 5
        t.send(b"7", 0.08)  # one edit: lim = 7
        txt = screen_text(t) or ""
        res["mid"] = _line_with(t, "big = ")
        res["mid_status"] = txt.split("\n")[-1]
        res["mid_fg"] = _fg_of(t, "big = ", "\u2248")
        t.send(b"\x1b", 0.8)  # Esc: cancel
        txt = screen_text(t) or ""
        res["cancel"] = _line_with(t, "big = ")
        res["cancel_status"] = txt.split("\n")[-1]
        t.pump(1.0)
        res["cancel_later"] = _line_with(t, "big = ")
        t.send(b"\x1b[1;2D", 0.2)
        t.send(b"8", 0.2)  # the next edit plans the job again (x > 8)
        t.pump(6.0)
        res["after"] = _line_with(t, "big = ")
        t.send(b"\x11", 0.3)
        t.send(b"d", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check("big = 900000" in res["before"], "wb stale: the value before", repr(res["before"]))
    check("big = \u2248900000" in res["mid"],
          "wb stale: while the job runs, the last value after the stale mark", repr(res["mid"]))
    check("recalculating" in res["mid_status"], "wb stale: the status says recalculating",
          repr(res["mid_status"]))
    check(bool(res["mid_fg"]) and res["mid_fg"][0] != res["mid_fg"][1],
          "wb stale: the stale value paints in its own (dim) colour", repr(res["mid_fg"]))
    check("big = \u2248900000" in res["cancel"] and "big = \u2248900000" in res["cancel_later"],
          "wb stale: Esc cancels, the stale mark stays", repr((res["cancel"], res["cancel_later"])))
    check("cancel" in res["cancel_status"], "wb stale: the status says cancelled",
          repr(res["cancel_status"]))
    check("big = 270000" in res["after"] and "\u2248" not in res["after"],
          "wb stale: the next edit recomputes; fresh again", repr(res["after"]))


def case_wb_anchor(exe, tmp):
    """Workbook anchors (docs/workbook.md "Fixed rows: anchors"): `{#name}`
    at a row's first cell stays visible in Rich and in Source, painted in
    its own accent colour; `T#name.col` reads the row; Copy Reference to
    Row on an anchored row says what it copied; Anchor Row on a row without
    one inserts `{#name}` (one undo step)."""
    if pyte is None:
        print("skip: wb anchor (no pyte)")
        return
    body = (b"# A\n\n```calc\nx = T#top.v * 10\n```\n\nTable: T\n\n| k | v |\n|---|---|\n"
            b"| {#top} alpha | 4 |\n| beta | 5 |\n")
    home = config_home(tmp, "cfg_anc")
    path = scratch_file(tmp, "anc.wb.md", body)
    # wide: the status line's message follows the key hints
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe"), "XDG_CONFIG_HOME": home},
            cols=200)
    t.pump(0.8)
    res = {}
    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.5)  # Ctrl-D: Rich on
        res["rich"] = _line_with(t, "alpha")
        res["rich_fg"] = _fg_of(t, "{#top}", "alpha")
        res["calc"] = _line_with(t, "x = ")
        t.send(b"\x04", 0.5)  # Source
        res["source"] = _line_with(t, "alpha")
        res["source_fg"] = _fg_of(t, "{#top}", "alpha")
        t.send(b"\x04", 0.5)  # Rich again
        # the caret on `beta`: Ctrl-End, Up, Home
        t.send(b"\x1b[1;5F", 0.3)
        t.send(b"\x1b[A\x1b[H\x1b[C\x1b[C\x1b[C", 0.3)
        t.send(F1, 0.4)
        t.send(b"copy reference to row", 0.4)
        t.send(b"\r", 0.5)
        res["noanchor_status"] = (screen_text(t) or "").split("\n")[-1]
        t.send(F1, 0.4)
        t.send(b"anchor row", 0.4)
        t.send(b"\r", 0.5)
        res["anchored"] = _line_with(t, "beta")
        t.send(F1, 0.4)
        t.send(b"copy reference to row", 0.4)
        t.send(b"\r", 0.5)
        res["copied_status"] = (screen_text(t) or "").split("\n")[-1]
        t.send(b"\x1a", 0.5)  # Ctrl-Z: one undo removes the anchor
        res["undone"] = _line_with(t, "beta")
        t.send(b"\x11", 0.3)
        t.send(b"d", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check("{#top} alpha" in res["rich"], "wb anchor: the tag stays visible in Rich", repr(res["rich"]))
    check(bool(res["rich_fg"]) and res["rich_fg"][0] != res["rich_fg"][1],
          "wb anchor: the tag paints in its own colour (Rich)", repr(res["rich_fg"]))
    check("{#top} alpha" in res["source"], "wb anchor: the tag is in Source", repr(res["source"]))
    check(bool(res["source_fg"]) and res["source_fg"][0] != res["source_fg"][1],
          "wb anchor: the tag paints in its own colour (Source)", repr(res["source_fg"]))
    check("x = 40" in res["calc"], "wb anchor: T#top.v reads the anchored row", repr(res["calc"]))
    check("no anchor" in res["noanchor_status"],
          "wb anchor: Copy Reference on a row without an anchor says so",
          repr(res["noanchor_status"]))
    check("{#beta}" in res["anchored"], "wb anchor: Anchor Row inserts {#name}", repr(res["anchored"]))
    check("T#beta." in res["copied_status"], "wb anchor: Copy Reference copies T#name.col",
          repr(res["copied_status"]))
    check("{#" not in res["undone"], "wb anchor: one undo removes it", repr(res["undone"]))


def case_image_placeholder(exe, tmp):
    """Images in a Rich Markdown pane (docs/images.md): the terminal paints
    the text stand-in `[image: alt WxH]` (the size read from the header in
    the background), inline too; the caret on the line shows the source;
    a remote image names its host and is not fetched; "Load Image" on it
    prompts, Esc cancels; the bytes on disk never change."""
    if pyte is None:
        print("skip: image placeholder (no pyte)")
        return
    import shutil
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.join(tmp, "imgproj")
    os.makedirs(os.path.join(proj, ".git"), exist_ok=True)
    shutil.copy(os.path.join(here, "..", "testdata", "img", "quad.png"), proj)
    body = (b"# Pics\n\n![a quad](quad.png)\n\nInline ![tiny](quad.png) here.\n\n"
            b"![far](http://127.0.0.1:9/x.png)\n\nend\n")
    path = os.path.join(proj, "doc.md")
    with open(path, "wb") as f:
        f.write(body)
    # tui_images off: every image is its text stand-in (the pictures have
    # their own cases below).
    t = Tui(exe, [path], {"RTX_SAFE_HOME": os.path.join(tmp, "safe_img"),
                          "RTX_TUI_IMAGES": "off"})
    shots = {}
    try:
        t.pump(1.0)
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.5)  # Ctrl-D: Rich on
        t.pump(0.5)
        shots["rich"] = screen_text(t)
        t.send(b"\x1b[B\x1b[B", 0.5)  # caret onto the image line
        shots["caret"] = screen_text(t)
        t.send(b"\x1b[B\x1b[B\x1b[B\x1b[B", 0.5)  # the remote image's line
        t.send(F1, 0.4)
        t.send(b"load image", 0.4)
        t.send(b"\r", 0.5)
        shots["ask"] = screen_text(t)
        t.send(b"\x1b", 0.5)
        shots["cancel"] = screen_text(t)
        t.send(b"\x11", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    with open(path, "rb") as f:
        disk = f.read()
    rich = shots["rich"] or ""
    check("[image: a quad 32x24]" in rich, "image: stand-in with the header size", repr(rich[:300]))
    check("Inline [image: tiny 32x24] here." in rich, "image: inline stand-in")
    check("[image: far - 127.0.0.1]" in rich, "image: remote names its host, not fetched")
    check("![a quad](quad.png)" in (shots["caret"] or ""), "image: caret on the line shows the source")
    check("remote image (127.0.0.1, http): 1 load" in (shots["ask"] or ""),
          "image: Load Image prompts for a remote image", repr((shots["ask"] or "")[-160:]))
    check("remote image (" not in (shots["cancel"] or "") and "Load Image" not in (shots["cancel"] or ""),
          "image: Esc cancels the prompt")
    check(disk == body, "image: bytes on disk unchanged")

def case_wb_uses(exe, tmp):
    """Workbook imports (docs/workbook.md "W1 phase 2 as built"): two panes,
    a.wb.md `uses q = "q.wb.md"` and reads q's names; q is open in the other
    pane. An edit in q's pane plans a job there; a's pane paints the
    imported value stale (`≈` and its last value) while q works, then the
    new value, fresh."""
    if pyte is None:
        print("skip: wb uses (no pyte)")
        return
    rows = b"".join(b"| %d |\n" % (i % 10) for i in range(300000))
    q = (b"# Q\n\n```params\nlim = 5\n```\n\n```calc\nbig = sum(T.x where x > lim)\n```\n\n"
         b"Table: T\n\n| x |\n|---|\n" + rows)
    a = (b"# A\n\n```uses\nq = \"q.wb.md\"\n```\n\n```calc\nimp = q.big\ntwice = imp * 2\n```\n")
    env = {"RTX_WB_INLINE_WORK": "512", "RTX_WB_SLICE_MS": "1", "RTX_WB_SLICE_ROWS": "64",
           "RTX_WB_SLICE_PAUSE_MS": "2"}  # q's job outlasts the keys below
    qp = scratch_file(tmp, "q.wb.md", q)
    ap = scratch_file(tmp, "a.wb.md", a)
    env["RTX_SAFE_HOME"] = os.path.join(tmp, "safe_uses")
    t = Tui(exe, [ap, qp], env, cols=200)
    t.pump(6.0)
    res = {}
    try:
        txt = screen_text(t) or ""
        if "rich" not in txt.split("\n")[-1]:
            t.send(b"\x04", 0.5)  # Ctrl-D: Rich on (a's pane)
        t.pump(1.5)
        res["before"] = _line_with(t, "twice = ")
        t.send(b"\x1b[17~", 0.3)  # F6: q's pane
        t.send(b"\x1b[1;5H", 0.3)  # Ctrl-Home
        t.send(b"\x1b[B\x1b[B\x1b[B\x1b[F", 0.3)  # `lim = 5`, its end
        t.send(b"\x1b[1;2D", 0.2)  # Shift-Left: select the 5
        t.send(b"7", 0.2)  # lim = 7: q's big rebuilds as a job
        res["mid"] = _line_with(t, "twice = ")
        res["mid_fg"] = _fg_of(t, "twice = ", "≈")
        t.pump(8.0)
        res["after"] = _line_with(t, "twice = ")
        t.send(b"\x11", 0.3)
        t.send(b"d", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    check("twice = 1800000" in res["before"], "wb uses: the imported value in the other pane",
          repr(res["before"]))
    check("twice = ≈1800000" in res["mid"],
          "wb uses: stale (the last value after the mark) while the used workbook works",
          repr(res["mid"]))
    check(bool(res["mid_fg"]) and res["mid_fg"][0] != res["mid_fg"][1],
          "wb uses: the stale import paints dimmed", repr(res["mid_fg"]))
    check("twice = 1020000" in res["after"] and "≈" not in res["after"],
          "wb uses: the edit reached the reader's pane; fresh again", repr(res["after"]))


def img_project(tmp, name, extra=b"", repo=True):
    """A project with quad.png (32x24, quadrants red / green / blue / white)
    and big.png (1200x900, the same quadrants) and a Markdown file that
    shows both as picture rows, one inline, and 40 lines after. repo=False:
    no repository marker (the document's directory is the root)."""
    import shutil
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.join(tmp, name)
    os.makedirs(os.path.join(proj, ".git") if repo else proj, exist_ok=True)
    for f in ("quad.png", "big.png", "anim.gif"):
        shutil.copy(os.path.join(here, "..", "testdata", "img", f), proj)
    body = (b"# Pics\n\n![a quad](quad.png)\n\nInline ![tiny](quad.png) here.\n\n"
            b"![big one](big.png)\n\n" + extra +
            b"".join(b"line %d\n" % i for i in range(40)))
    path = os.path.join(proj, "doc.md")
    with open(path, "wb") as f:
        f.write(body)
    return proj, path, body


def img_env(tmp, name, **kw):
    e = {"RTX_SAFE_HOME": os.path.join(tmp, "safe_" + name)}
    e.update(kw)
    return e


def fg_rgb(c):
    """pyte fg / bg of a cell as rgb (a 256 index 'iN' or a hex string)."""
    if c.startswith("i"):
        i = int(c[1:])
        h = pyte.graphics.FG_BG_256[i]
    elif c == "default":
        return None
    else:
        h = c
    return tuple(int(h[k:k + 2], 16) for k in (0, 2, 4))


def near(rgb, want, tol=60):
    return rgb is not None and all(abs(a - b) <= tol for a, b in zip(rgb, want))


def find_row(sc, needle):
    for y in range(sc.lines):
        if needle in "".join(sc.buffer[y][x].data for x in range(sc.columns)):
            return y
    return -1


def full_repaint_equal(t, fake, cell=(10, 20)):
    """The screen (text, colours, pixel layer) now, then after a focus-in
    (which clears and repaints every row): equal when nothing is stale."""
    def snap():
        sc = fake_screen(t.out, t.cols, t.rows, cell)
        cells = [[(sc.buffer[y][x].data, sc.buffer[y][x].fg, sc.buffer[y][x].bg)
                  for x in range(t.cols)] for y in range(t.rows)]
        return cells, {k: tuple(sorted(v.items())) for k, v in sc.pix.items()}
    a = snap()
    t.send(b"\x1b[I", 0.5)
    b = snap()
    rows = [y for y in range(t.rows) if a[0][y] != b[0][y]]
    pix = sorted(set(a[1]) ^ set(b[1]) | {k for k in a[1] if k in b[1] and a[1][k] != b[1][k]})
    return not rows and not pix, (rows[:4], pix[:4])


def kitty_cells(sc):
    """Every kitty placeholder cell: (x, y, image id, row, col)."""
    out = []
    for y in range(sc.lines):
        for x in range(sc.columns):
            c = sc.buffer[y][x]
            if not c.data.startswith("\U0010eeee"):
                continue
            d = [DIAC.index(ord(ch)) if ord(ch) in DIAC else -1 for ch in c.data[1:]]
            lo = int(c.fg[1:]) if c.fg.startswith("i") else -1
            msb = d[2] if len(d) > 2 else 0
            out.append((x, y, (msb << 24) | lo, d[0] if d else -1, d[1] if len(d) > 1 else -1))
    return out


DIAC = []


def load_diac():
    """kitty's row / column diacritics, read from core/img_term.c."""
    import re
    if DIAC:
        return
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, "..", "core", "img_term.c")).read()
    tab = src[src.index("g_diac[RTX_TIMG_PH_MAX] = {"):]
    tab = tab[:tab.index("};")]
    DIAC.extend(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]+)", tab))


def case_image_blocks(exe, tmp):
    """No graphics protocol answers (the harness is silent), TERM says 256
    colours: a Rich picture row is Unicode block art — ordinary cells —
    sized from the header at the assumed 8x16 cell; an inline image stays
    `[image: alt WxH]`; the caret on the line shows the source with the
    picture under it; truecolor (COLORTERM) uses 24-bit SGR; quadrants and
    sextants are settings; scrolling with and without scroll regions
    leaves exactly what a full repaint would."""
    if pyte is None:
        print("skip: image blocks (no pyte)")
        return
    proj, path, body = img_project(tmp, "blk")
    fake = FakeTerm("silent")
    t0 = time.time()
    t = Tui(exe, ["--no-blink", path], img_env(tmp, "blk"), fake=fake)
    first = None
    try:
        while time.time() - t0 < 2.0 and first is None:
            t.pump(0.02)
            if b"\x1b[?2026h" in t.out:
                first = time.time() - t0
        t.pump(1.2)
        sc = fake_screen(t.out, t.cols, t.rows)
        y = find_row(sc, " 3 ")
        cells = [sc.buffer[y][x] for x in range(3, 7)] if y >= 0 else []
        low = [sc.buffer[y + 1][x] for x in range(3, 7)] if y >= 0 else []
        txt = "\n".join(sc.display)
        check(first is not None and first < 0.8, "image blocks: first frame within the detection "
              "timeout (no replies)", repr(first))
        check(y >= 0 and all(c.data == "▀" for c in cells), "image blocks: half blocks",
              repr([c.data for c in cells]))
        check(y >= 0 and near(fg_rgb(cells[0].fg), (220, 30, 30)) and
              near(fg_rgb(cells[3].fg), (30, 200, 40)) and near(fg_rgb(low[0].fg), (40, 60, 220)),
              "image blocks: quadrant colours (256)", repr([(c.fg, c.bg) for c in cells + low]))
        check("Inline [image: tiny 32x24] here." in txt, "image blocks: inline stays text")
        check(b"\x1b_G" not in t.out[t.out.find(b"\x1b[?2026h"):] and b"\x1bPq" not in t.out,
              "image blocks: no graphics escapes")
        big = find_row(sc, " 7 ")
        check(big > 0 and near(fg_rgb(sc.buffer[big + 1][4].bg), (220, 30, 30)) and
              sc.buffer[big + 1][4].bg == sc.buffer[big + 2][4].bg,
              "image blocks: a big picture spans rows")
        t.send(b"\x1b[B\x1b[B", 0.6)  # caret onto the image's line
        sc2 = fake_screen(t.out, t.cols, t.rows)
        y2 = find_row(sc2, "![a quad](quad.png)")
        check(y2 >= 0 and sc2.buffer[y2 + 1][3].data == "▀",
              "image blocks: the caret shows the source, the picture under it")
        t.send(b"\x1b[A\x1b[A", 0.4)
        bad = []
        for keys in (b"\x1b[<65;10;10M", b"\x1b[<65;10;10M", b"\x1b[6~", b"\x1b[<64;10;10M",
                     b"\x1b[5~", b"\x1b[B" * 8):
            t.send(keys, 0.4)
            ok, why = full_repaint_equal(t, fake, (8, 16))
            if not ok:
                bad.append((keys[:8], why))
        check(not bad, "image blocks: scrolled frames equal a full repaint", repr(bad[:2]))
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    # --no-scroll-regions: the same pictures by whole-row rewrites.
    t = Tui(exe, ["--no-blink", "--no-scroll-regions", path], img_env(tmp, "blk2"),
            fake=FakeTerm("silent"))
    try:
        t.pump(1.2)
        bad = []
        for keys in (b"\x1b[<65;10;10M", b"\x1b[6~", b"\x1b[<64;10;10M", b"\x1b[5~"):
            t.send(keys, 0.4)
            ok, why = full_repaint_equal(t, t.fake, (8, 16))
            if not ok:
                bad.append((keys[:8], why))
        check(not bad, "image blocks: --no-scroll-regions frames equal a full repaint",
              repr(bad[:2]))
        check(b"\x1b[" not in b"".join(__import__("re").findall(rb"\x1b\[\d+;\d+r", t.out)),
              "image blocks: --no-scroll-regions sets no region")
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    # 24-bit, quadrants, sextants; no colours at all -> the text stand-in.
    for env, name, want in (({"COLORTERM": "truecolor"}, "24-bit", b"38;2;"),
                            ({"RTX_TUI_BLOCKS": "quadrant"}, "quadrants", None),
                            ({"RTX_TUI_BLOCKS": "sextant"}, "sextants", None),
                            ({"TERM": "xterm"}, "no colours", None)):
        e = img_env(tmp, "blk3")
        e.update(env)
        t = Tui(exe, ["--no-blink", path], e, fake=FakeTerm("silent", truecolor=False))
        try:
            t.pump(1.4)
            sc = fake_screen(t.out, t.cols, t.rows)
            y = find_row(sc, " 3 ")
            glyphs = "".join(sc.buffer[y][x].data for x in range(3, 7)) if y >= 0 else ""
            txt = "\n".join(sc.display)
            if want:
                check(want in bytes(t.out), "image blocks: %s SGR" % name)
            elif name == "quadrants":
                check(any(0x2596 <= ord(c[:1] or " ") <= 0x259F or c in "▀▄▌▐ "
                          for c in glyphs) and glyphs.strip() != "",
                      "image blocks: quadrant glyphs", repr(glyphs))
            elif name == "sextants":
                check(glyphs.strip() != "" and all(c == " " or 0x1FB00 <= ord(c[0]) <= 0x1FB3B or
                                                   c[0] in "▀▄▌▐█"
                                                   for c in glyphs if c),
                      "image blocks: sextant glyphs", repr(glyphs))
            else:
                check("[image: a quad 32x24]" in txt,
                      "image blocks: no colours -> the text stand-in", repr(txt[:200]))
            t.send(b"\x11", 0.3)
            t.wait_exit(5.0)
        finally:
            t.kill()


def case_image_kitty(exe, tmp):
    """A kitty terminal (fake): each picture is sent once (a=T, U=1 virtual
    placement, f=32, o=z, q=2; a temp file t=t when the terminal reads our
    files, chunked base64 otherwise) and then painted as Unicode
    placeholder cells whose id, row and column match; scrolling, a
    side-by-side split and a stacked split leave what a full repaint
    would; ^Z and exit delete every image sent, resume sends them again."""
    if pyte is None:
        print("skip: image kitty (no pyte)")
        return
    load_diac()
    proj, path, body = img_project(tmp, "kit")
    for ssh in (False, True):
        fake = FakeTerm("kitty")
        env = img_env(tmp, "kit")
        if ssh:
            env["SSH_TTY"] = "/dev/pts/99"
            env["RTX_TUI_IMAGES"] = "kitty"
        t = Tui(exe, ["--no-blink", path], env, fake=fake, jobctl=not ssh)
        try:
            t.pump(1.5)
            sent = [c for c in fake.cmds if c.get("a") == "T"]
            live = fake.images()
            sc = fake_screen(t.out, t.cols, t.rows)
            ph = kitty_cells(sc)
            tag = " (ssh, direct)" if ssh else ""
            check(len(sent) == 2 and all(c.get("U") == "1" and c.get("f") == "32" and
                                         c.get("o") == "z" and c.get("q") == "2" for c in sent),
                  "image kitty: two transmits with a virtual placement" + tag,
                  repr([{k: v for k, v in c.items() if k != "payload"} for c in sent]))
            if ssh:
                check(all(c.get("t", "d") == "d" for c in sent) and
                      any(c.get("m") == "1" for c in fake.cmds),
                      "image kitty: over SSH the data is chunked base64")
            else:
                check(all(c.get("t") == "t" for c in sent) and not any(
                    os.path.exists(p) for p in fake.files),
                    "image kitty: local transmits use temp files (read and deleted)")
            check(all(len(v[4]) == v[0] * v[1] * 4 for v in live.values()),
                  "image kitty: zlib data inflates to s x v x 4 bytes" + tag)
            ids = {i for (_, _, i, _, _) in ph}
            check(ph and ids <= set(live), "image kitty: placeholders name live images" + tag,
                  repr((sorted(ids), sorted(live))))
            ok = True
            for (x, y, i, r, c) in ph:
                x0 = min(px for (px, py, j, rr, cc) in ph if j == i and rr == r)
                y0 = min(py for (px, py, j, rr, cc) in ph if j == i)
                if i not in live or r != y - y0 or c != x - x0 or r >= live[i][3] or \
                        c >= live[i][2]:
                    ok = False
            check(ok, "image kitty: row / column diacritics match the cells" + tag)
            if not ssh:
                bad = []
                for keys in (b"\x1b[<65;10;10M", b"\x1b[6~", b"\x1b[<64;10;10M", b"\x1b[5~",
                             b"\x1c", b"\x1b[<65;60;10M", b"\x1d", b"\x1f",
                             b"\x1b[<65;10;18M", b"\x1d"):
                    t.send(keys, 0.5)
                    ok, why = full_repaint_equal(t, fake)
                    sc = fake_screen(t.out, t.cols, t.rows)
                    live = fake.images()
                    if not ok or not {i for (_, _, i, _, _) in kitty_cells(sc)} <= set(live):
                        bad.append((keys[:8], why))
                check(not bad, "image kitty: scroll / splits leave no stale cells", repr(bad[:2]))
                sent_ids = set(fake.sent())
                mark = len(fake.cmds)
                os.kill(t.target, signal.SIGTSTP)
                t.pump(0.4)
                dels = {int(c["i"]) for c in fake.cmds[mark:] if c.get("a") == "d"}
                check(dels >= set(fake.images()) and fake.images() == {},
                      "image kitty: ^Z deletes our images", repr((dels, sent_ids)))
                mark = len(fake.cmds)
                os.kill(t.target, signal.SIGCONT)
                t.pump(1.0)
                again = [c for c in fake.cmds[mark:] if c.get("a") == "T"]
                check(again and fake.images(), "image kitty: resume sends them again")
            t.send(b"\x11", 0.3)
            t.wait_exit(5.0)
            check(fake.images() == {} and fake.deletes(),
                  "image kitty: exit deletes every image sent" + tag,
                  repr((sorted(fake.images()), fake.deletes())))
        finally:
            t.kill()


def case_image_sixel(exe, tmp):
    """A sixel terminal (fake, DA1 attribute 4): pictures are drawn over
    blank cells after the rows, cropped to the pane; after every scroll
    (wheel, page, arrows), a side-by-side split and a close, the pixels
    and text equal a full repaint — nothing torn or stale; a region
    scroll never moves rows under a picture."""
    if pyte is None:
        print("skip: image sixel (no pyte)")
        return
    import re
    proj, path, body = img_project(tmp, "six")
    fake = FakeTerm("sixel")
    t = Tui(exe, ["--no-blink", path], img_env(tmp, "six"), fake=fake)
    try:
        t.pump(1.5)
        sc = fake_screen(t.out, t.cols, t.rows)
        y = find_row(sc, " 3 ")
        tl = sc.pix.get((3, y), {}) if y >= 0 else {}
        tr = sc.pix.get((6, y), {}) if y >= 0 else {}
        check(b"\x1bP0;1;0q" in t.out and sc.nimg >= 2, "image sixel: pictures drawn",
              repr(sc.nimg))
        check(tl and near(max(set(tl.values()), key=list(tl.values()).count), (220, 30, 30), 20)
              and tr and near(max(set(tr.values()), key=list(tr.values()).count),
                              (30, 200, 40), 20),
              "image sixel: quadrant colours in their cells")
        check(sc.buffer[y][3].data == " ", "image sixel: blank text under a picture")
        bad = []
        steps = (b"\x1b[<65;10;10M", b"\x1b[<65;10;10M", b"\x1b[6~", b"\x1b[<64;10;10M",
                 b"\x1b[5~", b"\x1b[B" * 9, b"\x1b[A" * 9, b"\x1c", b"\x1b[<65;60;10M",
                 b"\x1b[<64;60;10M", b"\x1d")
        for keys in steps:
            t.send(keys, 0.6)
            ok, why = full_repaint_equal(t, fake)
            if not ok:
                bad.append((keys[:8], why))
        check(not bad, "image sixel: frames equal a full repaint (no torn / stale pixels)",
              repr(bad[:3]))
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def art_cells(sc, t):
    """Cells painted as the quadrants' colours (block art: a glyph in the
    foreground, or a solid area as a blank on that background)."""
    quads = ((220, 30, 30), (30, 200, 40), (40, 60, 220))
    n = 0
    for y in range(t.rows):
        for x in range(t.cols):
            c = sc.buffer[y][x]
            for col in (c.fg, c.bg):
                try:
                    rgb = fg_rgb(col)
                except (ValueError, IndexError):
                    rgb = None
                if rgb and any(near(rgb, q, 30) for q in quads):
                    n += 1
                    break
    return n


def case_image_sixel_scroll(exe, tmp):
    """A sixel picture scrolled so the pane's top cuts it: each wheel step
    needs a crop that starts lower in the picture. While that crop is
    still encoding (a slow encoder here), its rows show the picture as
    block art, not blank; once it lands the sixel covers them and the
    frame equals a full repaint, with no block art left under it."""
    if pyte is None:
        print("skip: image sixel scroll (no pyte)")
        return
    proj, path, body = img_project(tmp, "sixs")
    fake = FakeTerm("sixel")
    t = Tui(exe, ["--no-blink", path], img_env(tmp, "sixs", RTX_TUI_ENC_DELAY_MS="700"),
            fake=fake)
    try:
        t.pump(3.0)
        for _ in range(5):
            t.send(b"\x1b[<65;10;10M", 0.05)
        t.pump(0.25)
        sc = fake_screen(t.out, t.cols, t.rows)
        art = art_cells(sc, t)
        check(art > 20, "image sixel scroll: block art while the crop encodes", repr(art))
        t.pump(3.0)
        sc = fake_screen(t.out, t.cols, t.rows)
        left = art_cells(sc, t)
        check(sc.nimg >= 1 and left == 0, "image sixel scroll: the sixel lands, no block art left",
              repr((sc.nimg, left)))
        ok, why = full_repaint_equal(t, fake)
        check(ok, "image sixel scroll: equals a full repaint", why)
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def case_image_iterm(exe, tmp):
    """iTerm2 (XTVERSION): OSC 1337 inline PNGs sized in cells, over blank
    cells; scrolling equals a full repaint."""
    if pyte is None:
        print("skip: image iterm (no pyte)")
        return
    import base64
    import re
    proj, path, body = img_project(tmp, "itm")
    fake = FakeTerm("iterm")
    t = Tui(exe, ["--no-blink", path], img_env(tmp, "itm"), fake=fake)
    try:
        t.pump(1.5)
        m = re.findall(rb"\x1b\]1337;File=inline=1;size=(\d+);width=(\d+);height=(\d+);"
                       rb"[^:]*:([A-Za-z0-9+/=]+)\x07", bytes(t.out))
        check(len(m) >= 2 and all(base64.b64decode(p)[:8] == b"\x89PNG\r\n\x1a\n" and
                                  len(base64.b64decode(p)) == int(s) for s, w, h, p in m),
              "image iterm: inline PNGs", repr([(s, w, h) for s, w, h, p in m]))
        check(any((w, h) == (b"4", b"2") for s, w, h, p in m), "image iterm: sized in cells")
        bad = []
        for keys in (b"\x1b[<65;10;10M", b"\x1b[6~", b"\x1b[5~"):
            t.send(keys, 0.6)
            ok, why = full_repaint_equal(t, fake)
            if not ok:
                bad.append((keys[:8], why))
        check(not bad, "image iterm: frames equal a full repaint", repr(bad[:2]))
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def case_image_tmux(exe, tmp):
    """Inside tmux: kitty graphics only when the passthrough echo comes back
    (every APC then wrapped in DCS tmux;), else block art."""
    if pyte is None:
        print("skip: image tmux (no pyte)")
        return
    proj, path, body = img_project(tmp, "tmx")
    for mode in ("passthrough", "closed"):
        fake = FakeTerm("kitty", tmux=mode)
        env = img_env(tmp, "tmx", TMUX="/tmp/tmux-0/default,1,0", TERM="tmux-256color")
        t = Tui(exe, ["--no-blink", path], env, fake=fake)
        try:
            t.pump(1.5)
            out = bytes(t.out)
            sent = [c for c in fake.cmds if c.get("a") == "T"]
            if mode == "passthrough":
                check(sent and all(c["tmux"] for c in sent) and
                      not __import__("re").search(rb"(?<!\x1b)\x1b_Ga=T", out),
                      "image tmux: kitty through passthrough (wrapped)")
            else:
                sc = fake_screen(t.out, t.cols, t.rows)
                y = find_row(sc, " 3 ")
                check(not sent and y >= 0 and sc.buffer[y][3].data == "▀",
                      "image tmux: passthrough off -> block art")
            t.send(b"\x11", 0.3)
            t.wait_exit(5.0)
        finally:
            t.kill()


def case_image_viewer(exe, tmp):
    """An image file opened directly is the viewer: the picture fitted to
    the pane (block art here) with its caption; + zooms, 1 is actual
    size, arrows pan, 0 fits again; typing never edits; Ctrl-D is its hex
    and back. The browse preview of an image file shows its picture over
    the size line."""
    if pyte is None:
        print("skip: image viewer (no pyte)")
        return
    proj, path, body = img_project(tmp, "vw")
    big = os.path.join(proj, "big.png")
    with open(big, "rb") as f:
        disk = f.read()
    fake = FakeTerm("silent")
    t = Tui(exe, ["--no-blink", big], img_env(tmp, "vw"), fake=fake)
    try:
        t.pump(1.5)
        sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
        txt = "\n".join(sc.display)
        # 1200x900 into 80 x 22 cells of 8x16: 10 rows of 16 = 352 px tall.
        mid = sc.buffer[5]
        check("PNG 1200x900" in txt and "(fit)" in txt, "image viewer: caption", repr(txt[-300:]))
        check(near(fg_rgb(mid[20].bg), (220, 30, 30)) and near(fg_rgb(mid[60].bg), (30, 200, 40)),
              "image viewer: the picture, fitted", repr((mid[20].bg, mid[60].bg)))
        t.send(b"1", 0.8)
        sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
        txt = "\n".join(sc.display)
        check("100%" in txt and near(fg_rgb(sc.buffer[5][40].bg), (220, 30, 30)),
              "image viewer: 1 = actual size (red corner fills the pane)")
        t.send(b"\x1b[C" * 30 + b"\x1b[B" * 20, 0.8)
        sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
        check(near(fg_rgb(sc.buffer[10][40].bg), (250, 250, 250)),
              "image viewer: arrows pan to the white corner",
              repr(sc.buffer[10][40].bg))
        t.send(b"abc\x7f\r", 0.4)
        t.send(b"0", 0.6)
        txt = "\n".join(fake_screen(t.out, t.cols, t.rows, (8, 16)).display)
        check("(fit)" in txt, "image viewer: 0 fits again")
        t.send(b"\x04", 0.6)
        txt = "\n".join(fake_screen(t.out, t.cols, t.rows, (8, 16)).display)
        check("89 50 4e 47" in txt.lower(), "image viewer: Ctrl-D shows the hex",
              repr(txt[:200]))
        t.send(b"\x04", 0.6)
        txt = "\n".join(fake_screen(t.out, t.cols, t.rows, (8, 16)).display)
        check("PNG 1200x900" in txt, "image viewer: Ctrl-D back to the picture")
        t.send(b"\x13", 0.3)
        t.send(b"\x11", 0.3)
        t.send(b"q", 0.2)
        t.wait_exit(5.0)
    finally:
        t.kill()
    with open(big, "rb") as f:
        check(f.read() == disk, "image viewer: bytes unchanged")
    # Browse preview: the picture, then the size line.
    t = Tui(exe, ["--no-blink", proj], img_env(tmp, "vw2"), fake=FakeTerm("silent"), cols=120)
    try:
        t.pump(1.0)
        for _ in range(8):
            sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
            if "big.png" in "\n".join(sc.display):
                break
            t.pump(0.3)
        # ../, anim.gif, big.png: two steps down.
        t.send(b"\x1b[B", 0.5)
        t.send(b"\x1b[B", 0.5)
        y = -1
        for _ in range(10):
            sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
            y = find_row(sc, "[image: PNG 1200x900")
            if y > 3:
                break
            t.pump(0.3)
        txt = "\n".join(sc.display)
        colored = [x for x in range(t.cols) if y > 3 and near(fg_rgb(sc.buffer[y - 3][x].bg),
                                                                  (250, 250, 250))]
        check(y > 3 and colored and min(colored) > t.cols // 2,
              "image preview: the picture above the size line", repr((y, txt[-600:])))
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def case_image_present(exe, tmp):
    """Terminal presentation: a slide's content image and a split
    `![bg left]` are pictures in the slide box (block art); a kitty
    terminal gets placeholder cells for them."""
    if pyte is None:
        print("skip: image present (no pyte)")
        return
    import shutil
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.join(tmp, "prs")
    os.makedirs(os.path.join(proj, ".git"), exist_ok=True)
    shutil.copy(os.path.join(here, "..", "testdata", "img", "big.png"), proj)
    deck = (b"---\nmarp: true\n---\n\n# One\n\n![pic w:400](big.png)\n\n---\n\n"
            b"![bg left:40%](big.png)\n\n# Two\n\ntext\n")
    path = os.path.join(proj, "deck.md")
    with open(path, "wb") as f:
        f.write(deck)
    for mode in ("silent", "kitty"):
        fake = FakeTerm(mode)
        t = Tui(exe, ["--no-blink", path], img_env(tmp, "prs"), fake=fake)
        cell = (10, 20) if mode == "kitty" else (8, 16)
        try:
            t.pump(1.0)
            t.send(b"\x1b[15;2~", 1.2)            # Shift-F5 on slide 1
            sc = fake_screen(t.out, t.cols, t.rows, cell)
            if mode == "silent":
                red = [(x, y) for y in range(t.rows) for x in range(t.cols)
                       if near(fg_rgb(sc.buffer[y][x].bg), (220, 30, 30))]
                check(red and "One" in "\n".join(sc.display),
                      "image present: a content picture (blocks)")
            else:
                load_diac()
                check(kitty_cells(sc) and fake.images(), "image present: kitty cells")
            t.send(b" ", 1.2)                      # slide 2: bg left
            sc = fake_screen(t.out, t.cols, t.rows, cell)
            if mode == "silent":
                left = [x for x in range(t.cols // 2)
                        if near(fg_rgb(sc.buffer[t.rows // 3][x].bg), (220, 30, 30)) or
                        near(fg_rgb(sc.buffer[t.rows // 3][x].bg), (30, 200, 40))]
                check(left and "Two" in "\n".join(sc.display),
                      "image present: ![bg left] is a picture on its side")
            t.send(b"\x1b", 0.5)
            t.send(b"\x11", 0.3)
            t.wait_exit(5.0)
            if mode == "kitty":
                check(fake.images() == {}, "image present: kitty images deleted on exit")
        finally:
            t.kill()


def main_wakeups(pid):
    """Context switches of the main (UI) thread: the editor's own loop
    (an animation's frame budget; idle counts every thread, check_idle)."""
    n = 0
    try:
        with open("/proc/%d/task/%d/status" % (pid, pid)) as f:
            for line in f:
                if line.startswith(("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches")):
                    n += int(line.split()[1])
    except OSError:
        pass
    return n


def case_idle_threads(exe, tmp):
    """Every thread idle once work settles (check_idle): a Markdown file
    with pictures (terminal pictures off too), a workbook, a Marp deck
    presenting a picture slide, and browse with a Markdown preview open."""
    if pyte is None or not os.path.exists("/proc/self/task"):
        print("skip: idle threads (no pyte or /proc)")
        return
    import shutil
    here = os.path.dirname(os.path.abspath(__file__))
    proj, path, body = img_project(tmp, "idt", repo=False)
    shutil.copy(os.path.join(here, "..", "testdata", "wb", "revenue.wb.md"), proj)
    deck = os.path.join(proj, "deck.md")
    with open(deck, "wb") as f:
        f.write(b"---\nmarp: true\n---\n\n# One\n\n![pic w:400](big.png)\n\n---\n\n"
                b"![bg left:40%](big.png)\n\n# Two\n\ntext\n")
    big = os.path.join(proj, "big.wb.md")
    with open(big, "w") as f:
        f.write("# Big\n\nTable: T\n\n| id | amount | cost | margin |\n|----|----|----|----|\n")
        for i in range(60000):
            f.write("| %d | %d.50 | %d.25 | `=@amount - @cost` |\n" % (i, i % 997, i % 13))
        f.write("\n```calc\ntotal = sum(T.margin)\n```\n")
    runs = [("markdown with pictures", [path], {}, b""),
            ("markdown, pictures off", [path], {"RTX_TUI_IMAGES": "off"}, b""),
            ("workbook", [os.path.join(proj, "revenue.wb.md")], {}, b""),
            ("big workbook (read job)", [big], {}, b""),
            ("Marp deck presenting", [deck], {}, b"\x1b[15;2~"),
            ("browse preview", [proj], {}, b"doc.md")]
    for tag, args, extra, keys in runs:
        t = Tui(exe, ["--no-blink"] + args, img_env(tmp, "idt", **extra),
                fake=FakeTerm("silent"))
        try:
            t.pump(1.0)
            if keys:
                t.send(keys, 1.0)
            check_idle("idle threads: " + tag, t.pid, t.pump)
            t.send(b"\x1b", 0.2)
            t.send(b"\x11", 0.3)
        finally:
            t.kill()


def case_image_idle(exe, tmp):
    """Pictures on screen and nothing to do: no output, no wakeups (block
    art and kitty). With image_animate on, an animated GIF paints its
    frames (at most ~12 a second) and a still file stays quiet again once
    it scrolls away."""
    if pyte is None or not os.path.exists("/proc/self/stat"):
        print("skip: image idle (no pyte or /proc)")
        return
    proj, path, body = img_project(tmp, "idl", extra=b"![spin](anim.gif)\n\n", repo=False)
    for mode in ("silent", "kitty"):
        fake = FakeTerm(mode)
        t = Tui(exe, ["--no-blink", path], img_env(tmp, "idl"), fake=fake)
        try:
            t.pump(1.5)
            c0 = proc_cpu(t.pid)
            mark = len(t.out)
            check_idle("image idle: %s pictures on screen" % mode, t.pid, t.pump)
            nbytes = len(t.out) - mark
            c1 = proc_cpu(t.pid)
            check(nbytes == 0 and c1 - c0 < 0.05,
                  "image idle: %s pictures on screen, nothing to do" % mode,
                  repr((nbytes, c1 - c0)))
            t.send(b"\x11", 0.3)
            t.wait_exit(5.0)
        finally:
            t.kill()
    home = config_home(tmp, "idl_cfg", settings='{ "image_animate": true }')
    spin = os.path.join(proj, "spin.md")
    with open(spin, "wb") as f:
        f.write(b"# A\n\n![spin](anim.gif)\n\nend\n")
    t = Tui(exe, ["--no-blink", spin], img_env(tmp, "idl2", XDG_CONFIG_HOME=home),
            fake=FakeTerm("silent"))
    try:
        t.pump(1.5)
        seen = set()
        for _ in range(14):
            t.pump(0.1)
            sc = fake_screen(t.out, t.cols, t.rows, (8, 16))
            seen.add(sc.buffer[2][2].bg)  # line 3 (a two-column gutter)
        check(len(seen) >= 2, "image idle: image_animate paints the GIF's frames", repr(seen))
        w0 = main_wakeups(t.pid)
        t.pump(1.0)
        wakes = main_wakeups(t.pid) - w0
        check(0 < wakes <= 30, "image idle: animation wakes within the frame budget",
              repr(wakes))
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def case_mermaid_blocks(exe, tmp):
    """A ```mermaid fence in the terminal (docs/images.md "Mermaid"):
    RTX_TUI_IMAGES=blocks, so the diagram cctext-render draws is Unicode
    block art in ordinary cells; the fence's lines are hidden (the picture
    row is its opening line); the caret into the fence shows the source
    with the diagram under it; typing keeps the old diagram up (darkened)
    until the new one lands; nothing to do once it settles."""
    if pyte is None:
        print("skip: mermaid blocks (no pyte)")
        return
    if not os.path.exists(os.path.join(os.path.dirname(exe), "cctext-render")):
        print("skip: mermaid blocks (no cctext-render)")
        return
    proj = os.path.join(tmp, "mmb")
    os.makedirs(os.path.join(proj, ".git"), exist_ok=True)
    body = (b"# Diagram\n\nBefore.\n\n```mermaid\nflowchart LR\n  A[Start] --> B{Ok?}\n"
            b"  B --> C[Done]\n```\n\nAfter the fence.\n" +
            b"".join(b"line %d\n" % i for i in range(30)))
    path = os.path.join(proj, "doc.md")
    with open(path, "wb") as f:
        f.write(body)
    env = img_env(tmp, "mmb")
    env["RTX_TUI_IMAGES"] = "blocks"
    t = Tui(exe, ["--no-blink", path], env, fake=FakeTerm("silent"))

    def art_rows(sc, y0, y1):
        """Rows in [y0, y1) holding block glyphs."""
        n = 0
        for y in range(max(0, y0), min(sc.lines, y1)):
            row = "".join(sc.buffer[y][x].data for x in range(sc.columns))
            if any(ch in row for ch in "▀▄█"):
                n += 1
        return n
    try:
        t0 = time.time()
        while time.time() - t0 < 2.0 and b"\x1b[?2026h" not in t.out:
            t.pump(0.02)
        t.pump(0.05)
        sc = fake_screen(t.out, t.cols, t.rows)
        txt = "\n".join(sc.display)
        check("[diagram: rendering...]" in txt or art_rows(sc, 4, 20) > 0,
              "mermaid blocks: a one-line box while it renders", txt[:400])
        t.pump(3.0)
        sc = fake_screen(t.out, t.cols, t.rows)
        txt = "\n".join(sc.display)
        top = find_row(sc, " 5 ")
        after = find_row(sc, "After the fence.")
        check(top >= 0 and after > top + 4 and art_rows(sc, top, after) >= 4,
              "mermaid blocks: the diagram is block art under line 5",
              repr((top, after, art_rows(sc, top, after))))
        check("flowchart LR" not in txt and "```mermaid" not in txt,
              "mermaid blocks: the fence's lines are hidden")
        frames = t.out[t.out.find(b"\x1b[?2026h"):]
        check(b"\x1b_G" not in frames and b"\x1bPq" not in frames,
              "mermaid blocks: no graphics escapes (cells only)")
        # Settled: nothing to do.
        mark = len(t.out)
        c0 = proc_cpu(t.pid) if os.path.exists("/proc/self/stat") else 0
        check_idle("mermaid blocks: settled", t.pid, t.pump)
        c1 = proc_cpu(t.pid) if os.path.exists("/proc/self/stat") else 0
        check(len(t.out) == mark and c1 - c0 < 0.05, "mermaid blocks: idle, no output",
              repr((len(t.out) - mark, c1 - c0)))
        # The caret into the fence: the source, the diagram under it.
        t.send(b"\x1b[B" * 4, 0.8)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, " 9 ```")
        after = find_row(sc, "After the fence.")
        check(find_row(sc, "flowchart LR") >= 0 and close >= 0 and after > close + 3 and
              art_rows(sc, close + 1, after) >= 4,
              "mermaid blocks: the caret shows the source, the diagram under it",
              repr((close, after)))
        # Type into the fence: the old diagram stays (darkened), then the new.
        # Looked at well inside the new render (a flowchart takes ~200 ms
        # warm): its size lands a moment before its pixels, and the box
        # re-fits in that gap.
        t.send(b"\x1b[B\x1b[B\x1b[B\x1b[F", 0.3)
        t.send(b"\r  C --> D[More]", 0.12)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, "10 ```")
        check(close > 0 and art_rows(sc, close + 1, close + 14) >= 4,
              "mermaid blocks: the old diagram stays up while the source changes",
              "\n".join(sc.display))
        t.pump(3.0)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, "10 ```")
        check(close > 0 and art_rows(sc, close + 1, close + 14) >= 4 and
              "[diagram" not in "\n".join(sc.display),
              "mermaid blocks: the new diagram lands", "\n".join(sc.display))
        t.send(b"\x11", 0.3)
        t.send(b"n", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


def case_math_blocks(exe, tmp):
    """Math in the terminal (docs/images.md "Math"): RTX_TUI_IMAGES=blocks,
    so a `$$` block is Unicode block art under its line with the block's
    lines hidden; the caret into it shows the source with the formula
    under it; typing keeps the old formula up until the new one lands;
    inline math stays source (math_unicode off), or becomes Unicode text
    for a simple formula with math_unicode on; nothing to do once
    settled."""
    if pyte is None:
        print("skip: math blocks (no pyte)")
        return
    if not os.path.exists(os.path.join(os.path.dirname(exe), "cctext-render")):
        print("skip: math blocks (no cctext-render)")
        return
    proj = os.path.join(tmp, "mtb")
    os.makedirs(os.path.join(proj, ".git"), exist_ok=True)
    body = (b"# Formula\n\nInline $x^2 + 1$ and $\\alpha \\le \\beta$ here.\n\n$$\n"
            b"x = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}\n$$\n\nAfter the block.\n" +
            b"".join(b"line %d\n" % i for i in range(30)))
    path = os.path.join(proj, "doc.md")
    with open(path, "wb") as f:
        f.write(body)
    env = img_env(tmp, "mtb")
    env["RTX_TUI_IMAGES"] = "blocks"
    t = Tui(exe, ["--no-blink", path], env, fake=FakeTerm("silent"))

    def art_rows(sc, y0, y1):
        n = 0
        for y in range(max(0, y0), min(sc.lines, y1)):
            row = "".join(sc.buffer[y][x].data for x in range(sc.columns))
            if any(ch in row for ch in "▀▄█"):
                n += 1
        return n
    try:
        t0 = time.time()
        while time.time() - t0 < 2.0 and b"\x1b[?2026h" not in t.out:
            t.pump(0.02)
        t.pump(3.0)
        sc = fake_screen(t.out, t.cols, t.rows)
        txt = "\n".join(sc.display)
        top = find_row(sc, " 5 ")
        after = find_row(sc, "After the block.")
        check(top >= 0 and after > top + 2 and art_rows(sc, top, after) >= 2,
              "math blocks: the formula is block art under line 5",
              repr((top, after, art_rows(sc, top, after))) + "\n" + txt)
        check("\\frac" not in txt and "$$" not in txt,
              "math blocks: the block's lines are hidden")
        # Rich hides the dollars as it hides other marks' delimiters.
        check("x^2 + 1" in txt and "\\alpha \\le \\beta" in txt,
              "math blocks: inline math stays source (math_unicode off)")
        frames = t.out[t.out.find(b"\x1b[?2026h"):]
        check(b"\x1b_G" not in frames and b"\x1bPq" not in frames,
              "math blocks: no graphics escapes (cells only)")
        mark = len(t.out)
        c0 = proc_cpu(t.pid) if os.path.exists("/proc/self/stat") else 0
        check_idle("math blocks: settled", t.pid, t.pump)
        c1 = proc_cpu(t.pid) if os.path.exists("/proc/self/stat") else 0
        check(len(t.out) == mark and c1 - c0 < 0.05, "math blocks: idle, no output",
              repr((len(t.out) - mark, c1 - c0)))
        # The caret into the block: the source, the formula under it.
        t.send(b"\x1b[B" * 5, 0.8)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, " 7 $$")
        after = find_row(sc, "After the block.")
        check(find_row(sc, "\\frac{-b") >= 0 and close >= 0 and after > close + 2 and
              art_rows(sc, close + 1, after) >= 2,
              "math blocks: the caret shows the source, the formula under it",
              repr((close, after)) + "\n" + "\n".join(sc.display))
        # Type into the formula: the old picture stays, then the new one.
        t.send(b"\x1b[F + y", 0.12)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, " 7 $$")
        check(close > 0 and art_rows(sc, close + 1, close + 8) >= 2,
              "math blocks: the old formula stays up while the source changes",
              "\n".join(sc.display))
        t.pump(2.0)
        sc = fake_screen(t.out, t.cols, t.rows)
        close = find_row(sc, " 7 $$")
        check(close > 0 and art_rows(sc, close + 1, close + 8) >= 2 and
              "[math" not in "\n".join(sc.display),
              "math blocks: the new formula lands", "\n".join(sc.display))
        t.send(b"\x11", 0.3)
        t.send(b"n", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    # math_unicode: a simple inline formula as Unicode text.
    sp = os.path.join(proj, "settings.json")
    with open(sp, "w") as f:
        f.write('{"math_unicode": true}')
    env = img_env(tmp, "mtu")
    env["RTX_TUI_IMAGES"] = "off"
    env["RTX_SETTINGS"] = sp
    t = Tui(exe, ["--no-blink", path], env, fake=FakeTerm("silent"))
    try:
        t.pump(1.5)
        sc = fake_screen(t.out, t.cols, t.rows)
        txt = "\n".join(sc.display)
        check("x\u00b2 + 1" in txt and "\u03b1 \u2264 \u03b2" in txt and "$x^2" not in txt,
              "math unicode: simple inline math as Unicode text", txt[:600])
        check("\\frac{-b" in txt, "math unicode: no pictures here, the block stays source")
        t.send(b"\x11", 0.3)
        t.send(b"n", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()


# ---- themes (README "Settings", core/theme.cch) ----------------------------

LIGHT_GUTTER = b"\x1b[38;5;243m"   # the light theme's line numbers / greys
DARK_GUTTER = b"\x1b[90m"


def in_light(case):
    """Run a colour-checking case again with RTX_THEME=light: the same
    properties must hold on the light palette (its own SGRs)."""
    def run(exe, tmp):
        os.environ["RTX_THEME"] = "light"
        PREFIX[0] = "light: "
        try:
            d = os.path.join(tmp, "light_" + case.__name__)
            os.makedirs(d, exist_ok=True)
            case(exe, d)
        finally:
            os.environ.pop("RTX_THEME", None)
            PREFIX[0] = ""
    return run


def theme_run(exe, tmp, name, fake=None, env=None, args=(), keys=None):
    """Open a small file, optionally send keys; the bytes written."""
    body = b"alpha\nbeta\n  gamma\n"
    path = scratch_file(tmp, name, body)
    e = {"RTX_SAFE_HOME": os.path.join(tmp, "safe_" + name), "RTX_TUI_IMAGES": "off",
         "XDG_CONFIG_HOME": os.path.join(tmp, "cfg_" + name)}
    if env:
        e.update(env)
    t = Tui(exe, ["--no-blink"] + list(args) + [path], e, fake=fake)
    try:
        t.pump(1.0)
        first = bytes(t.out)
        for k in keys or ():
            t.send(k, 0.4)
        after = bytes(t.out[len(first):])
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    return first, after


def is_light_out(b):
    return LIGHT_GUTTER in b and DARK_GUTTER not in b


def is_dark_out(b):
    return DARK_GUTTER in b and LIGHT_GUTTER not in b


def case_theme_osc11(exe, tmp):
    """Theme auto in a terminal: the startup detection asks OSC 11; a light
    background (any X11 spec form, ST or BEL) picks the light palette, a
    dark one the dark palette, no answer falls back to COLORFGBG, then to
    dark. --theme / RTX_THEME beat the terminal. Images off still ask."""
    rows = (
        ("white 16-bit", FakeTerm("none", bg=b"rgb:ffff/ffff/ffff"), None, (), "light"),
        ("cream 8-bit BEL", FakeTerm("none", bg=b"rgb:fd/f6/e3", bel=True), None, (), "light"),
        ("black", FakeTerm("none", bg=b"rgb:0000/0000/0000"), None, (), "dark"),
        ("solarized dark #hex", FakeTerm("none", bg=b"#002b36"), None, (), "dark"),
        ("no answer", FakeTerm("none"), None, (), "dark"),
        ("no answer, COLORFGBG light", FakeTerm("none"), {"COLORFGBG": "0;15"}, (), "light"),
        ("no answer, COLORFGBG dark", FakeTerm("none"), {"COLORFGBG": "15;default;0"}, (), "dark"),
        ("silent terminal", FakeTerm("silent"), None, (), "dark"),
        ("light terminal, --theme=dark", FakeTerm("none", bg=b"rgb:ffff/ffff/ffff"), None,
         ("--theme=dark",), "dark"),
        ("dark terminal, RTX_THEME=light", FakeTerm("none", bg=b"rgb:0000/0000/0000"),
         {"RTX_THEME": "light"}, (), "light"),
    )
    for i, (label, fake, env, args, want) in enumerate(rows):
        t0 = time.time()
        first, _ = theme_run(exe, tmp, "th%d.txt" % i, fake=fake, env=env, args=args)
        asked = b"\x1b]11;?\x1b\\" in first
        got = "light" if is_light_out(first) else "dark" if is_dark_out(first) else "?"
        check(asked, "theme %s: OSC 11 asked" % label)
        check(got == want, "theme %s: %s palette" % (label, want), "got %s" % got)
        if label == "silent terminal":
            # No DA1 either: the detection budget bounds the wait.
            check(time.time() - t0 < 4.0, "theme: a silent terminal costs only the budget")


def case_theme_toggle(exe, tmp):
    """Toggle Light/Dark (the palette) repaints every row in the other
    palette, and back."""
    cfg = config_home(tmp, "cfg_toggle")
    first, after = theme_run(exe, tmp, "tog.txt", fake=FakeTerm("none"),
                             env={"XDG_CONFIG_HOME": cfg},
                             keys=(F1, b"Toggle Light", b"\r"))
    check(is_dark_out(first), "toggle: starts dark")
    check(LIGHT_GUTTER in after and b"alpha" in after,
          "toggle: Toggle Light/Dark repaints in the light palette", repr(after[-400:]))
    first, after = theme_run(exe, tmp, "tog2.txt", fake=FakeTerm("none", bg=b"rgb:ffff/ffff/ffff"),
                             env={"XDG_CONFIG_HOME": cfg},
                             keys=(F1, b"Toggle Light", b"\r"))
    check(is_light_out(first), "toggle: a light terminal starts light")
    check(DARK_GUTTER in after and b"alpha" in after,
          "toggle: and flips to dark", repr(after[-400:]))


def case_theme_settings(exe, tmp):
    """settings.json "theme": "light" wins over a dark terminal; a bad
    value is reported and ignored."""
    sp = os.path.join(tmp, "theme_settings.json")
    with open(sp, "w") as f:
        f.write('{"theme": "light"}')
    first, _ = theme_run(exe, tmp, "ts.txt", fake=FakeTerm("none", bg=b"rgb:0000/0000/0000"),
                         env={"RTX_SETTINGS": sp})
    check(is_light_out(first), "settings theme light: light palette in a dark terminal")
    with open(sp, "w") as f:
        f.write('{"theme": "sepia"}')
    first, _ = theme_run(exe, tmp, "ts2.txt", fake=FakeTerm("none"), env={"RTX_SETTINGS": sp})
    check(b"theme is dark, light or auto" in first and is_dark_out(first),
          "settings theme: a bad value is reported, auto stays")


def case_theme_light_colours(exe, tmp):
    """The light palette in cells (pyte): a selection is a light blue
    background (256-colour 153), not ANSI blue under dark text; a grid's
    header row is a light grey band (254), not the dark theme's 236."""
    if pyte is None:
        print("skip: theme light colours (no pyte)")
        return

    def row_of(sc, t, needle):
        for y in range(t.rows):
            row = "".join(sc.buffer[y][x].data for x in range(t.cols))
            if needle in row:
                return y, row
        return -1, ""

    body = b"# t\n\nplain words here\n"
    t, _ = open_tui(exe, tmp, "lc.md", body, env={"RTX_THEME": "light"})
    try:
        t.send(b"\x1b[B\x1b[B", 0.3)
        t.send(b"\x1b[1;2C" * 5, 0.4)  # Shift-Right x5: select "plain"
        sc = t.screen()
        y, row = row_of(sc, t, "plain words")
        x = row.find("plain")
        sel_bg = sc.buffer[y][x + 1].bg if y >= 0 and x >= 0 else None
        t.send(b"\x11", 0.3)
        t.send(b"d", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    t, _ = open_tui(exe, tmp, "lc.csv", b"name,value\nalpha,1\nbeta,2\n", args=("--grid",),
                    env={"RTX_THEME": "light"})
    try:
        sc = t.screen()
        y, row = row_of(sc, t, "name")
        x = row.find("name")
        hdr_bg = sc.buffer[y][x].bg if y >= 0 and x >= 0 else None
        t.send(b"\x11", 0.3)
        t.wait_exit(5.0)
    finally:
        t.kill()
    # pyte names 256-colour cells by hex: 153 = afd7ff, 254 = e4e4e4
    check(sel_bg == "afd7ff", "light: the selection is a light blue background", repr(sel_bg))
    check(hdr_bg == "e4e4e4", "light: a grid header is a light grey band", repr(hdr_bg))


CASES = {
    "theme_osc11": case_theme_osc11,
    "theme_toggle": case_theme_toggle,
    "theme_settings": case_theme_settings,
    "theme_light_colours": case_theme_light_colours,
    "math_blocks": case_math_blocks,
    "image_idle": case_image_idle,
    "idle_threads": case_idle_threads,
    "image_viewer": case_image_viewer,
    "image_present": case_image_present,
    "image_blocks": case_image_blocks,
    "mermaid_blocks": case_mermaid_blocks,
    "image_kitty": case_image_kitty,
    "image_sixel": case_image_sixel,
    "image_sixel_scroll": case_image_sixel_scroll,
    "image_iterm": case_image_iterm,
    "image_tmux": case_image_tmux,
    "present": case_present,
    "image_placeholder": case_image_placeholder,
    "tabs_and_panes": case_tabs_and_panes,
    "bracketed_paste": case_bracketed_paste,
    "paste_split_reads": case_paste_split_reads,
    "paste_one_undo": case_paste_one_undo,
    "unbracketed_typing": case_unbracketed_typing,
    "dangling_csi": case_dangling_csi,
    "page_keys": case_page_keys,
    "last_line": case_last_line,
    "mouse_click": case_mouse_click,
    "find_run": case_find_run,
    "find_options": case_find_options,
    "find_replace": case_find_replace,
    "esc_help": case_esc_help,
    "esc_fast": case_esc_fast,
    "stats_json_clean": case_stats_json_clean,
    "sigterm_restores": case_sigterm_restores,
    "sigsegv_restores": case_sigsegv_restores,
    "suspend_resume": case_suspend_resume,
    "focus_repaint": case_focus_repaint,
    "incremental_is_full": case_incremental_is_full,
    "scroll_regions_equal_full": case_scroll_regions_equal_full,
    "clip_osc52": case_clip_osc52,
    "clip_quiet": case_clip_quiet,
    "stats_idle": case_stats_idle,
    "blink_idle_stops": case_blink_idle_stops,
    "blink_unfocused_stops": case_blink_unfocused_stops,
    "no_blink_idle": case_no_blink_idle,
    "cursor_blinks_itself": case_cursor_blinks_itself,
    "cursor_soft_blink_vscode": case_cursor_soft_blink_vscode,
    "cursor_is_caret": case_cursor_is_caret,
    "cursor_rich": case_cursor_rich,
    "cursor_split_and_prompts": case_cursor_split_and_prompts,
    "cursor_hex": case_cursor_hex,
    "cursor_grid": case_cursor_grid,
    "cursor_browse": case_cursor_browse,
    "browse_crumb": case_browse_crumb,
    "caret_cell_fallback": case_caret_cell_fallback,
    "cursor_restored": case_cursor_restored,
    "md_enter_continues": case_md_enter_continues,
    "md_enter_empty_exits": case_md_enter_empty_exits,
    "md_tab_indent": case_md_tab_indent,
    "md_autopair": case_md_autopair,
    "md_highlight": case_md_highlight,
    "md_smart_paste": case_md_smart_paste,
    "md_table_col_delete": case_md_table_col_delete,
    "md_table_fit": case_md_table_fit,
    "palette_run": case_palette_run,
    "palette_chords": case_palette_chords,
    "keymap_file": case_keymap_file,
    "keymap_flag": case_keymap_flag,
    "settings_file": case_settings_file,
    "quick_open": case_quick_open,
    "project_search": case_project_search,
    "wb_annotation": case_wb_annotation,
    "wb_deferred": case_wb_deferred,
    "wb_stale": case_wb_stale,
    "wb_anchor": case_wb_anchor,
    "wb_uses": case_wb_uses,
    # colour-checking cases again on the light palette
    "md_highlight_light": in_light(case_md_highlight),
    "wb_annotation_light": in_light(case_wb_annotation),
    "wb_stale_light": in_light(case_wb_stale),
    "wb_anchor_light": in_light(case_wb_anchor),
}


def main(argv):
    exe = "bin/cctext"
    names = []
    for a in argv[1:]:
        if os.path.sep in a or a.endswith("cctext"):
            exe = a
        else:
            names.append(a)
    exe = os.path.abspath(exe)
    if not names:
        names = list(CASES)
    with tempfile.TemporaryDirectory(prefix="cctext_pty_") as tmp:
        for n in names:
            CASES[n](exe, tmp)
    print("%d failed" % len(FAILS))
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

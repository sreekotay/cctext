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


class Tui:
    """jobctl: run the editor as a foreground job below a waiting parent,
    the way a shell does, so SIGTSTP can stop it (a session leader's own
    group is orphaned and the kernel drops stop signals to it)."""

    def __init__(self, exe, args, env_extra=None, rows=ROWS, cols=COLS,
                 jobctl=False):
        self.rows, self.cols = rows, cols
        self.out = bytearray()
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(os.environ)
            env["TERM"] = "xterm-256color"
            for k in ("TMUX", "TERM_PROGRAM", "WAYLAND_DISPLAY", "DISPLAY",
                      "SSH_TTY", "SSH_CONNECTION"):
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

    def send(self, data, settle=0.25):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.fd, data)
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
        st.feed(bytes(self.out))
        return sc


FAILS = []


def check(cond, name, detail=""):
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


CASES = {
    "present": case_present,
    "tabs_and_panes": case_tabs_and_panes,
    "bracketed_paste": case_bracketed_paste,
    "paste_split_reads": case_paste_split_reads,
    "paste_one_undo": case_paste_one_undo,
    "unbracketed_typing": case_unbracketed_typing,
    "dangling_csi": case_dangling_csi,
    "page_keys": case_page_keys,
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

#!/usr/bin/env python3
"""Head-to-head terminal editor benchmark (pty + pyte virtual screen).

    python3 run.py gen   [--dir DIR] [--sizes small_c,text_3m,json_100m,json_1g,json_2g]
    python3 run.py bench [--dir DIR] [--editors cctext,vim,...] [--fixtures ...]
                         [--repeats 5] [--keys 50] [--pages 20] [--out results/]
    python3 run.py probe EDITOR FILE [--keys-after-open '<python bytes literal>']
    python3 run.py versions

Each repeat launches the editor fresh in a COLSxROWS pty, feeds its output
into a pyte screen, and times until the *screen* shows the expected
content (not until bytes arrive). See README.md for methodology.
"""
import argparse
import ast
import datetime as dt
import fcntl
import json
import os
import re
import select
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time

import pyte

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import editors as E  # noqa: E402

COLS, ROWS = 200, 50
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = E.REPO
KEY_BUDGET_MS = 20000  # stop typing once this much key latency has accrued
MEM_GUARD = int(os.environ.get("BENCH_MEM_GUARD_MB", "10240")) * 1024  # kB

# --------------------------------------------------------------------------
# fixtures


def gen_small_c(path, target=50 * 1024):
    out = ["/* synthetic C source for the editor benchmark (generated) */",
           "#include <stdio.h>", "#include <stdlib.h>", ""]
    b = 0
    size = sum(len(s) + 1 for s in out)
    while size < target:
        blk = [
            "/* block %d: accumulate and clamp */" % b,
            "static int fn_%d(int a, int b) {" % b,
            "    int x = a * %d + b;" % b,
            "    if (x > %d) {" % (b * 3 + 7),
            "        return x - %d; // clamp" % b,
            "    }",
            '    printf("fn_%d %%d\\n", x);' % b,
            "    return x;",
            "}",
            "",
        ]
        size += sum(len(s) + 1 for s in blk)
        out += blk
        b += 1
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


FIXTURES = {
    # name: (filename, kind, generator description)
    "small_c": ("small.c", "c", "synthetic 50 KiB C (numbered functions)"),
    "text_3m": ("large_3M.txt", "text", "gen_large.sh --bytes 3M (prose+code blocks)"),
    "json_100m": ("large_100M.json", "json", "gen_large.sh --bytes 100M --json"),
    "json_1g": ("large_1G.json", "json", "gen_large.sh --bytes 1G --json"),
    "json_2g": ("large_2G.json", "json", "gen_large.sh --bytes 2G --json"),
}


def gen(args):
    os.makedirs(args.dir, exist_ok=True)
    gl = os.path.join(REPO, "testdata", "gen_large.sh")
    for name in args.sizes.split(","):
        fn, kind, _ = FIXTURES[name]
        p = os.path.join(args.dir, fn)
        if os.path.exists(p):
            print("exists", p)
            continue
        if name == "small_c":
            gen_small_c(p)
        elif kind == "text":
            subprocess.check_call([gl, "--bytes", "3M", p])
        else:
            size = fn.split("_")[1].split(".")[0]
            subprocess.check_call([gl, "--bytes", size, "--json", p])
        print("wrote", p, os.path.getsize(p))


def count_lines(path):
    n = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 24)
            if not b:
                break
            n += b.count(b"\n")
    return n


class Fixture:
    """Expected screen content for one file.

    ids: every content line carries a record/block number; `id_re`
    extracts it. first paint = id 0 and a later id on screen; mid = ids
    near the middle; search lands when the target's id line is visible.
    """

    def __init__(self, name, path):
        self.name, self.path = name, path
        fn, self.kind, self.desc = FIXTURES[name]
        self.size = os.path.getsize(path)
        self.lines = count_lines(path)
        if self.kind == "json":
            # line 1 is "[", line k (k>=2) holds id k-2
            self.id_re = re.compile(r'"id": (\d+),')
            self.nrec = self.lines - 2
            self.paint_re = [re.compile(r'"id": 0,'), re.compile(r'"id": 30,')]
            last = self.nrec - 6
            self.search_pat = 'item_%d"' % last
            # anchored so a hit-list / prompt row does not count as landed
            self.search_re = re.compile(r'(?m)^[\s\d|:>]*\{"id": %d,' % last)
            self.mid_line = self.lines // 2
            self.mid_id = self.mid_line - 2
            self.lines_per_id = 1
        else:
            if self.kind == "c":
                self.id_re = re.compile(r"(?:block |fn_|a \* )(\d+)")
                per = 10
                self.paint_re = [re.compile(r"block 0:"), re.compile(r"block 3:")]
            else:  # gen_large text: 8-line blocks
                self.id_re = re.compile(r"(?:Block |fn_|block |x = )(\d+)")
                per = 8
                self.paint_re = [re.compile(r"Block 0\."), re.compile(r"block 4\b")]
            with open(path, "rb") as f:
                f.seek(max(0, self.size - 4096))
                tail = f.read().decode("utf-8", "replace")
            last = max(int(m) for m in re.findall(r"fn_(\d+)\(", tail))
            self.nrec = last + 1
            tgt = last - 2
            # "fn_N" without "(" (regex engines would read a group); unique
            # because N has the maximum digit count in the file.
            self.search_pat = "fn_%d" % tgt
            self.search_re = re.compile(
                r"(?m)^[\s\d|:>]*(?:(?:/\* ?)?block %d[: ]|(?:static )?int fn_%d\()" % (tgt, tgt))
            self.mid_line = self.lines // 2
            self.mid_id = self.nrec // 2
            self.lines_per_id = per
        self.mid_tol = max(3, int(self.nrec * 0.02))

    def ids(self, disp):
        s = set()
        for row in disp:
            for m in self.id_re.finditer(row):
                s.add(int(m.group(1)))
        return s


# --------------------------------------------------------------------------
# terminal


class RScreen(pyte.Screen):
    """pyte screen that answers DA / DSR queries back into the pty."""

    def __init__(self, cols, rows, fd):
        super().__init__(cols, rows)
        self._fd = fd

    def select_graphic_rendition(self, *attrs, private=False, **kw):
        # CSI > Ps m (xterm modifyOtherKeys etc.) is not an SGR
        if private:
            return
        super().select_graphic_rendition(*attrs)

    def write_process_input(self, data):
        try:
            os.write(self._fd, data.encode())
        except OSError:
            pass


def tree_pids(pid):
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        try:
            for t in os.listdir("/proc/%d/task" % p):
                with open("/proc/%d/task/%s/children" % (p, t)) as f:
                    todo += [int(x) for x in f.read().split()]
        except OSError:
            pass
    return out


def mem_kb(pid):
    rss = hwm = 0
    for p in tree_pids(pid):
        try:
            with open("/proc/%d/status" % p) as f:
                for ln in f:
                    if ln.startswith("VmRSS:"):
                        rss += int(ln.split()[1])
                    elif ln.startswith("VmHWM:"):
                        hwm += int(ln.split()[1])
        except OSError:
            pass
    return rss, hwm


def sched_stats(pid):
    """(context switches, CPU seconds) summed over the process tree and
    all its threads. A context switch of a sleeping thread is a wakeup."""
    cs, ticks = 0, 0
    for p in tree_pids(pid):
        try:
            for t in os.listdir("/proc/%d/task" % p):
                with open("/proc/%d/task/%s/status" % (p, t)) as f:
                    for ln in f:
                        if ln.startswith(("voluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:")):
                            cs += int(ln.split()[1])
            with open("/proc/%d/stat" % p) as f:
                st = f.read().rsplit(")", 1)[1].split()
            ticks += int(st[11]) + int(st[12])  # utime + stime
        except (OSError, IndexError, ValueError):
            pass
    return cs, ticks / os.sysconf("SC_CLK_TCK")


class Timeout(Exception):
    pass


class Died(Exception):
    pass


class Term:
    def __init__(self, argv, env, cwd):
        self.master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.screen = RScreen(COLS, ROWS, self.master)
        self.stream = pyte.ByteStream(self.screen)
        self.t0 = time.perf_counter()

        def pre():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                     env=env, cwd=cwd, preexec_fn=pre, close_fds=True)
        os.close(slave)
        fl = fcntl.fcntl(self.master, fcntl.F_GETFL)
        fcntl.fcntl(self.master, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.last_out = self.t0
        self.dirty = True
        self._disp = None
        self.prompts = []
        self.peak_seen = 0
        self.guard_hit = False

    @property
    def pid(self):
        return self.proc.pid

    def alive(self):
        return self.proc.poll() is None

    def pump(self, wait):
        """Read whatever is available (waiting up to `wait` s). Returns the
        perf_counter timestamp of the read, or None if nothing arrived."""
        r, _, _ = select.select([self.master], [], [], wait)
        if not r:
            return None
        t = time.perf_counter()
        chunks = []
        while True:
            try:
                b = os.read(self.master, 1 << 16)
            except BlockingIOError:
                break
            except OSError:
                if not chunks:
                    raise Died()
                break
            if not b:
                break
            chunks.append(b)
        if not chunks:
            if not self.alive():
                raise Died()
            return None
        data = b"".join(chunks)
        self._answer_queries(data)
        self.stream.feed(data)
        self.last_out = t
        self._disp = None
        return t

    # Queries pyte does not answer. Unanswered, some editors wait out a
    # timeout (vim delays exit ~100 ms). Replies mimic a dark xterm.
    QUERIES = [
        (re.compile(rb"\x1b\[>0?c"), b"\x1b[>41;380;0c"),
        (re.compile(rb"\x1b\]11;\?(?:\x07|\x1b\\)"), b"\x1b]11;rgb:0000/0000/0000\x1b\\"),
        (re.compile(rb"\x1b\]10;\?(?:\x07|\x1b\\)"), b"\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
    ]

    def _answer_queries(self, data):
        if b"\x1b" not in data:
            return
        for rx, ans in self.QUERIES:
            for _ in rx.finditer(data):
                try:
                    os.write(self.master, ans)
                except OSError:
                    pass

    @property
    def disp(self):
        if self._disp is None:
            self._disp = list(self.screen.display)
        return self._disp

    def text(self):
        return "\n".join(self.disp)

    def send(self, data):
        os.write(self.master, data)

    def _guard(self):
        rss, _ = mem_kb(self.pid)
        self.peak_seen = max(self.peak_seen, rss)
        if rss > MEM_GUARD:
            self.guard_hit = True
            raise Timeout("mem-guard")

    def wait_for(self, pred, timeout):
        """Pump until pred(self) is true. Returns elapsed seconds from the
        time the call started to the read that made pred true."""
        start = time.perf_counter()
        deadline = start + timeout
        next_guard = start + 0.5
        if pred(self):
            return 0.0
        while True:
            now = time.perf_counter()
            if now > deadline:
                raise Timeout("timeout")
            if now > next_guard:
                self._guard()
                next_guard = now + 0.5
            t = self.pump(min(0.05, max(0.0, deadline - now)))
            if t is None:
                if not self.alive():
                    raise Died()
                continue
            for rx, ans in self.prompts:
                if rx.search(self.text()):
                    self.send(ans)
                    self.prompts = [p for p in self.prompts if p[0] is not rx]
            if pred(self):
                return t - start

    def settle(self, quiet=0.25, cap=10.0):
        """Pump until no output for `quiet` seconds (max `cap`)."""
        end = time.perf_counter() + cap
        while time.perf_counter() < end:
            if self.pump(quiet) is None:
                return
        return

    def run_script(self, steps, timeout=10.0):
        for s in steps:
            if isinstance(s, tuple) and s[0] == "wait":
                rx = re.compile(s[1])
                self.wait_for(lambda t: rx.search(t.text()), s[2] or timeout)
            else:
                self.send(s)
                # small gap so multi-key chords are not read as one paste
                self.pump(0.02)

    def colors(self, rows=None):
        """Distinct non-default foreground colours on non-blank cells of
        the content rows (heuristic: >=2 means highlighting is on)."""
        seen = set()
        rows = rows or range(1, ROWS - 2)
        for y in rows:
            line = self.screen.buffer[y]
            for x in range(COLS):
                ch = line[x]
                if ch.data.strip() and ch.fg != "default":
                    seen.add(ch.fg)
        return len(seen)

    def kill(self):
        if self.alive():
            try:
                os.killpg(self.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                self.proc.wait(5)
            except Exception:
                pass
        try:
            os.close(self.master)
        except OSError:
            pass

    def wait_exit(self, timeout):
        start = time.perf_counter()
        while time.perf_counter() - start < timeout:
            if self.proc.poll() is not None:
                return time.perf_counter() - start
            try:
                self.pump(0.002)
            except Died:
                pass
        raise Timeout("timeout")


# --------------------------------------------------------------------------
# a session


def editor_env(ed, home):
    env = {
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, ".config"),
        "XDG_DATA_HOME": os.path.join(home, ".local/share"),
        "XDG_STATE_HOME": os.path.join(home, ".local/state"),
        "XDG_CACHE_HOME": os.path.join(home, ".cache"),
        "TERM": "xterm-256color",
        "COLORTERM": "truecolor",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    }
    env.update(ed.env(home))
    return env


def timeouts(fx):
    gb = fx.size / (1 << 30)
    big = max(1.0, gb)
    return {
        "open": 20 + 90 * big,
        "op": 10 + 60 * big,
        "key": 5 + 5 * big,
        "quit": 10 + 30 * big,
    }


def session(ed, fx, home, nkeys, npages, log, idle=0.0):
    to = timeouts(fx)
    r = {}
    argv = ed.argv(fx.path, home)
    term = Term(argv, editor_env(ed, home), home)
    term.prompts = [(re.compile(rx), ans) for rx, ans in ed.open_prompts]

    def step(name, fn):
        if name in r:
            return
        try:
            r[name] = fn()
        except Timeout as e:
            r[name] = "OOM-GUARD" if str(e) == "mem-guard" else "TIMEOUT"
            raise
        except Died:
            r[name] = "DIED"
            raise

    try:
        # 1. first meaningful paint (spawn -> first + later line visible)
        def paint():
            dtm = term.wait_for(lambda t: all(rx.search(t.text()) for rx in fx.paint_re), to["open"])
            return (time.perf_counter() - term.t0) if dtm == 0 else (term.last_out - term.t0)
        step("open_ms", lambda: 1000 * paint())
        r["colors_at_paint"] = term.colors()
        term.settle(0.3, cap=3)
        rss, hwm = mem_kb(term.pid)
        r["rss_open_kb"], r["hwm_open_kb"] = rss, hwm
        r["colors_settled"] = term.colors()

        # 1b. idle: leave the editor alone for `idle` s after open and count
        # its wakeups (context switches), CPU time and output writes
        if idle > 0:
            cs0, cpu0 = sched_stats(term.pid)
            t0, writes = time.perf_counter(), 0
            while time.perf_counter() - t0 < idle:
                if term.pump(min(0.5, max(0.0, idle - (time.perf_counter() - t0)))) is not None:
                    writes += 1
            dt_ = time.perf_counter() - t0
            cs1, cpu1 = sched_stats(term.pid)
            r["idle_wakeups_s"] = (cs1 - cs0) / dt_
            r["idle_cpu_pct"] = 100 * (cpu1 - cpu0) / dt_
            r["idle_writes"] = writes

        # 2. scroll: PageDown x npages, each until the max visible id grows
        per = []
        for i in range(npages):
            term.settle(0.15, cap=2)  # let the previous page finish drawing
            before = max(fx.ids(term.disp) or {-1})
            # a page counts once the view advanced by >= half a screen, so a
            # partial frame (or the first of several wheel events) is not enough
            need = before + max(1, int(0.5 * ROWS / fx.lines_per_id))
            term.send(ed.pagedown)
            per.append(1000 * term.wait_for(
                lambda t: max(fx.ids(t.disp) or {-1}) >= need, to["key"]))
        r["pgdn_p50_ms"] = statistics.median(per)
        r["pgdn_total_ms"] = sum(per)
        term.settle(0.2, cap=2)

        # 3. search for a string near EOF
        def do_search():
            pre, timed = ed.search(fx.search_pat)
            term.run_script(pre)
            term.settle(0.1, cap=2)
            t = time.perf_counter()
            term.run_script(timed, timeout=to["op"])
            term.wait_for(lambda tt: fx.search_re.search("\n".join(ed.content(tt.disp))), to["op"])
            return 1000 * (term.last_out - t)
        step("search_ms", do_search)
        term.settle(0.3, cap=3)

        # 4. jump to the middle
        def mid_ok(t):
            ids = fx.ids(t.disp)
            return sum(1 for i in ids if abs(i - fx.mid_id) <= fx.mid_tol) >= 3
        def do_jump():
            pre, timed = ed.goto(fx.mid_line, 50)
            term.run_script(pre)
            term.settle(0.1, cap=2)
            t = time.perf_counter()
            term.run_script(timed, timeout=to["op"])
            term.wait_for(mid_ok, to["op"])
            return 1000 * (term.last_out - t)
        step("jump_mid_ms", do_jump)
        term.settle(0.3, cap=3)
        r["colors_mid"] = term.colors()

        # 5. keystroke latency: type Q x nkeys at the mid-file caret
        if ed.insert_enter:
            term.send(ed.insert_enter)
            term.settle(0.2, cap=2)
        base = term.text().count("Q")
        lat = []
        for k in range(1, nkeys + 1):
            t = time.perf_counter()
            term.send(b"Q")
            term.wait_for(lambda tt: tt.text().count("Q") >= base + k, to["key"])
            lat.append(1000 * (term.last_out - t) if term.last_out >= t else 0.0)
            term.settle(0.03, cap=2)  # drain the rest of this frame
            if sum(lat) > KEY_BUDGET_MS:
                break  # very slow editor: stop early, keep what we measured
        r["keys_typed"] = len(lat)
        lat.sort()
        r["key_p50_ms"] = statistics.median(lat)
        r["key_p95_ms"] = lat[min(len(lat) - 1, int(round(0.95 * (len(lat) - 1))))]
        r["key_max_ms"] = lat[-1]
        term.settle(0.2, cap=2)
        rss, hwm = mem_kb(term.pid)
        r["rss_end_kb"], r["hwm_end_kb"] = rss, hwm

        # 6. quit, discarding the edit
        if ed.insert_exit:
            term.send(ed.insert_exit)
            term.settle(0.3, cap=3)
        def do_quit():
            steps = ed.quit_discard()
            t = time.perf_counter()
            term.run_script(steps, timeout=to["quit"])
            term.wait_exit(to["quit"])
            return 1000 * (time.perf_counter() - t)
        step("quit_ms", do_quit)
    except (Timeout, Died) as e:
        r.setdefault("error", str(e) or type(e).__name__)
        r["screen_at_error"] = [ln.rstrip() for ln in term.disp]
        if term.proc.poll() is not None:
            r["exit_status"] = term.proc.returncode  # negative = killed by signal
        log("    ! %s: %s" % (ed.name, r.get("error")))
    except Exception as e:  # harness bug: keep going
        r["error"] = "harness: %r" % (e,)
        log("    ! harness error %r" % (e,))
    finally:
        r["peak_rss_seen_kb"] = max(term.peak_seen, r.get("hwm_end_kb", 0) or 0,
                                    r.get("hwm_open_kb", 0) or 0)
        term.kill()
    return r


# --------------------------------------------------------------------------
# reporting

METRICS = [
    ("open_ms", "open→paint ms"),
    ("rss_open_kb", "RSS open MiB"),
    ("pgdn_p50_ms", "PgDn p50 ms"),
    ("search_ms", "search→EOF ms"),
    ("jump_mid_ms", "jump 50% ms"),
    ("key_p50_ms", "key p50 ms"),
    ("key_p95_ms", "key p95 ms"),
    ("hwm_end_kb", "peak RSS MiB"),
    ("quit_ms", "quit ms"),
    ("colors_settled", "hl colours"),
]
# only shown when a cell measured them (`bench --idle SECS`)
IDLE_METRICS = [
    ("idle_wakeups_s", "idle wakeups/s"),
    ("idle_cpu_pct", "idle CPU %"),
]


def summarize(runs):
    out = {}
    for key, _ in METRICS + IDLE_METRICS + [("idle_writes", ""), ("colors_mid", ""), ("pgdn_total_ms", ""), ("key_max_ms", "")]:
        vals = [r.get(key) for r in runs if key in r]
        nums = [v for v in vals if isinstance(v, (int, float))]
        strs = [v for v in vals if isinstance(v, str)]
        if nums and len(nums) >= len(strs):
            out[key] = statistics.median(nums)
        elif strs:
            out[key] = strs[0]
    errs = [r["error"] for r in runs if "error" in r]
    if errs:
        out["errors"] = errs
    return out


def fmt(key, v):
    if v is None:
        return "—"
    if isinstance(v, str):
        return v
    if key.endswith("_kb"):
        return "%.1f" % (v / 1024)
    if key.startswith("colors"):
        return "%d" % v
    if v >= 100:
        return "%.0f" % v
    if v >= 10:
        return "%.1f" % v
    return "%.2f" % v


def write_markdown(res, path):
    L = []
    meta = res["meta"]
    L.append("# Editor head-to-head — %s\n" % meta["date"])
    L.append("Host: %s. PTY %dx%d, TERM=xterm-256color. Repeats: %s; keys per run: %d; "
             "PageDown x %d. Page cache warm (each file `cat`'d before its runs). "
             "Median across repeats.\n" % (meta["host"], COLS, ROWS, meta["repeats"], meta["keys"], meta["pages"]))
    L.append("## Versions\n")
    for n, v in meta["versions"].items():
        L.append("- **%s**: %s" % (n, v))
    cb = meta.get("builds", {}).get("cctext")
    if cb:
        L.append("\ncctext numbers below come from commit **%s** (%s)%s, binary built %s. "
                 "Each cell's own build is in the JSON (`fixtures.<fx>.cctext.build`)."
                 % (cb.get("commit"), cb.get("subject"), " + uncommitted changes" if cb.get("dirty") else "",
                    cb.get("bin_mtime")))
    L.append("")
    if meta.get("notes"):
        L.append("## Notes on this run\n")
        L += ["- " + n for n in meta["notes"]]
        L.append("")
    L.append("## Overview (median; open→paint ms / RSS after open MiB / key p50 ms)\n")
    fxn = list(res["fixtures"].keys())
    eds = []
    for fxres in res["fixtures"].values():
        eds += [e for e in fxres if e not in eds]
    L.append("| editor | " + " | ".join(fxn) + " |")
    L.append("|---|" + "---|" * len(fxn))
    for ed in eds:
        cells = []
        for f in fxn:
            s = res["fixtures"][f].get(ed, {}).get("summary")
            if not s:
                cells.append("—")
                continue
            cells.append("%s / %s / %s" % (fmt("open_ms", s.get("open_ms")),
                                            fmt("rss_open_kb", s.get("rss_open_kb")),
                                            fmt("key_p50_ms", s.get("key_p50_ms"))))
        L.append("| %s | " % ed + " | ".join(cells) + " |")
    L.append("")
    L.append("`hl colours` = distinct syntax colours on screen after open (0-1 = no highlighting). "
             "TIMEOUT / OOM-GUARD / DIED end that session; later columns stay empty.\n")
    for fxname, fxres in res["fixtures"].items():
        fm = res["fixture_meta"][fxname]
        L.append("## %s — %s (%.1f MiB, %d lines)\n" % (fxname, fm["desc"], fm["size"] / 2**20, fm["lines"]))
        mets = METRICS + [m for m in IDLE_METRICS
                          if any(m[0] in er["summary"] for er in fxres.values())]
        L.append("| editor | " + " | ".join(h for _, h in mets) + " |")
        L.append("|---|" + "---|" * len(mets))
        for ed, er in fxres.items():
            s = er["summary"]
            L.append("| %s | " % ed + " | ".join(fmt(k, s.get(k)) for k, _ in mets) + " |")
        L.append("")
        notes = []
        for ed, er in fxres.items():
            if er["summary"].get("errors"):
                notes.append("- %s: %s" % (ed, "; ".join(sorted(set(er["summary"]["errors"])))))
        if notes:
            L.append("Errors / timeouts:\n")
            L += notes
            L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def build_info(ed, stamp):
    """Version string, plus git commit / dirty flag / binary mtime for
    cctext so every cctext number names the build that produced it."""
    info = {"version": versions([ed]).get(ed.name), "bin": ed.bin}
    if ed.name == "cctext":
        def git(*a):
            try:
                return subprocess.run(["git", "-C", REPO] + list(a), capture_output=True,
                                      text=True, timeout=10).stdout.strip()
            except Exception:
                return ""
        info["commit"] = git("rev-parse", "--short", "HEAD")
        info["subject"] = git("log", "-1", "--format=%s")
        # git() strips the output, which can eat the first line's leading
        # status column, so take the path as the last field
        dirty = [ln for ln in git("status", "--porcelain").splitlines()
                 if ln.strip() and not ln.split()[-1].startswith("bench/")]
        info["dirty"] = bool(dirty)
        try:
            info["bin_mtime"] = dt.datetime.fromtimestamp(
                os.path.getmtime(ed.bin)).isoformat(timespec="seconds")
        except OSError:
            pass
    return info


def versions(eds):
    out = {}
    for ed in eds:
        try:
            p = subprocess.run(ed.version_argv(), capture_output=True, text=True, timeout=10,
                               env=dict(os.environ, TERM="dumb"), stdin=subprocess.DEVNULL)
            txt = (p.stdout or p.stderr).strip().splitlines()
            out[ed.name] = (txt[0] if txt else "?") + "  (%s)" % ed.bin
        except Exception as e:
            out[ed.name] = "unavailable: %r" % (e,)
    return out


def wait_quiet(log, max_load, patience=1800):
    """Other work on the host skews every number: wait (up to `patience`
    s) for the 1-minute load average to drop below max_load. Returns the
    load average at the start of the cell."""
    t0 = time.time()
    warned = False
    while os.getloadavg()[0] > max_load and time.time() - t0 < patience:
        if not warned:
            log("  (waiting for host load %.1f < %.1f)" % (os.getloadavg()[0], max_load))
            warned = True
        time.sleep(10)
    return os.getloadavg()[0]


def bench(args):
    eds = E.get(args.editors)
    fxs = [Fixture(n, os.path.join(args.dir, FIXTURES[n][0])) for n in args.fixtures.split(",")
           if os.path.exists(os.path.join(args.dir, FIXTURES[n][0]))]
    stamp = dt.datetime.now().strftime("%Y-%m-%d")
    if args.into:
        # merge mode: re-measure only the selected editors, keep every other
        # cell (and the metadata of editors not re-run) from that file
        jpath = args.into
        args.out = os.path.dirname(os.path.abspath(jpath))
    else:
        jpath = os.path.join(args.out, "%s%s.json" % (stamp, args.tag))
    os.makedirs(args.out, exist_ok=True)
    mpath = jpath[:-5] + ".md"
    logf = open(jpath[:-5] + ".log", "a")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")
        logf.flush()

    res = {"meta": {"date": stamp, "host": "%s, %d cpu, %.0f GiB RAM" % (
                        os.uname().release, os.cpu_count(),
                        os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2**30),
                    "repeats": args.repeats, "keys": args.keys, "pages": args.pages,
                    "versions": versions(eds), "cols": COLS, "rows": ROWS},
           "fixture_meta": {}, "fixtures": {}}
    if os.path.exists(jpath) and (args.resume or args.into):
        with open(jpath) as f:
            old = json.load(f)
        res["fixtures"] = old.get("fixtures", {})
        res["fixture_meta"] = old.get("fixture_meta", {})
        ov = old.get("meta", {}).get("versions", {})
        ov.update(res["meta"]["versions"])
        res["meta"]["versions"] = ov
        res["meta"]["date"] = old.get("meta", {}).get("date", stamp)
        res["meta"].setdefault("builds", old.get("meta", {}).get("builds", {}))
        if old.get("meta", {}).get("notes"):
            res["meta"]["notes"] = old["meta"]["notes"]
    res["meta"].setdefault("builds", {})
    for ed in eds:
        res["meta"]["builds"][ed.name] = build_info(ed, stamp)
    for fx in fxs:
        res["fixture_meta"][fx.name] = {"path": fx.path, "size": fx.size, "lines": fx.lines,
                                        "desc": fx.desc, "search": fx.search_pat,
                                        "mid_line": fx.mid_line, "mid_id": fx.mid_id}
        subprocess.run("cat '%s' > /dev/null" % fx.path, shell=True)
        reps = args.repeats if fx.size < (512 << 20) else max(1, args.big_repeats)
        for ed in eds:
            if ed.name in res["fixtures"].get(fx.name, {}) and args.resume and not args.into:
                continue
            if not (ed.bin and os.path.exists(ed.bin)):
                log("skip %s (no binary)" % ed.name)
                continue
            load0 = wait_quiet(log, args.max_load, args.patience)
            home = tempfile.mkdtemp(prefix="edbench-%s-" % ed.name)
            ed.setup(home)
            runs = []
            log("%s / %s" % (fx.name, ed.name))
            for i in range(reps):
                r = session(ed, fx, home, args.keys, args.pages, log, args.idle)
                runs.append(r)
                log("  run %d: %s" % (i, {k: (round(v, 2) if isinstance(v, float) else v)
                                           for k, v in r.items() if k != "screen_at_error"}))
                # an editor that cannot open this size will not do better next time
                if r.get("open_ms") in ("TIMEOUT", "OOM-GUARD", "DIED"):
                    break
            shutil.rmtree(home, ignore_errors=True)
            res["fixtures"].setdefault(fx.name, {})[ed.name] = {
                "runs": runs, "summary": summarize(runs),
                "measured": dt.datetime.now().isoformat(timespec="seconds"),
                "build": res["meta"]["builds"].get(ed.name),
                "loadavg_before": load0, "loadavg_after": os.getloadavg()[0]}
            with open(jpath, "w") as f:
                json.dump(res, f, indent=1)
            write_markdown(res, mpath)
    log("wrote %s and %s" % (jpath, mpath))


def probe(args):
    """Launch one editor on one file, optionally send keys, print the screen."""
    ed = E.get(args.editor)[0]
    home = tempfile.mkdtemp(prefix="edprobe-")
    ed.setup(home)
    term = Term(ed.argv(args.file, home), editor_env(ed, home), home)
    term.prompts = [(re.compile(rx), ans) for rx, ans in ed.open_prompts]
    term.settle(args.wait, cap=30)
    for k in args.keys:
        term.send(ast.literal_eval(k))
        term.settle(args.wait, cap=30)
    print("\n".join(ln.rstrip() for ln in term.disp))
    print("--- colors:", term.colors(), "alive:", term.alive(), "mem:", mem_kb(term.pid))
    term.kill()
    shutil.rmtree(home, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    default_dir = os.environ.get("BENCH_FIXTURES", "/tmp/edbench-fixtures")
    g = sub.add_parser("gen")
    g.add_argument("--dir", default=default_dir)
    g.add_argument("--sizes", default="small_c,text_3m,json_100m,json_1g")
    b = sub.add_parser("bench")
    b.add_argument("--dir", default=default_dir)
    b.add_argument("--editors", default=None)
    b.add_argument("--fixtures", default="small_c,text_3m,json_100m,json_1g,json_2g")
    b.add_argument("--repeats", type=int, default=5)
    b.add_argument("--big-repeats", type=int, default=3)
    b.add_argument("--keys", type=int, default=50)
    b.add_argument("--pages", type=int, default=20)
    b.add_argument("--out", default=os.path.join(HERE, "results"))
    b.add_argument("--tag", default="")
    b.add_argument("--resume", action="store_true")
    b.add_argument("--idle", type=float, default=0,
                   help="after open, leave the editor idle this many s and record wakeups/s and CPU %%")
    b.add_argument("--into", default=None,
                   help="merge into this results JSON: re-measure --editors on --fixtures, keep all other cells")
    b.add_argument("--max-load", type=float, default=1.5,
                   help="wait for the 1-min load average to drop below this before each cell")
    b.add_argument("--patience", type=float, default=1800,
                   help="max seconds to wait for --max-load before running anyway")
    p = sub.add_parser("probe")
    p.add_argument("editor")
    p.add_argument("file")
    p.add_argument("keys", nargs="*")
    p.add_argument("--wait", type=float, default=0.5)
    sub.add_parser("versions")
    rp = sub.add_parser("report")
    rp.add_argument("json")
    args = ap.parse_args()
    if args.cmd == "gen":
        gen(args)
    elif args.cmd == "bench":
        bench(args)
    elif args.cmd == "probe":
        probe(args)
    elif args.cmd == "report":
        with open(args.json) as f:
            res = json.load(f)
        write_markdown(res, args.json[:-5] + ".md")
    else:
        for k, v in versions(E.get()).items():
            print("%-8s %s" % (k, v))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Per-keystroke frame time in cctext-ui (X11: Xvfb + xdotool).

    python3 bench/ui_type_bench.py [path/to/cctext-ui] [--chars N] [--delay MS]
                                   [--shots DIR] [--only NAME]

For each fixture (a C file, a Markdown file in Rich view, a file of long
lines, a 400-row Rich table whose cells wrap) the GUI opens it, the caret moves into the text, and xdotool types
N characters. RTX_UI_FRAME_LOG (frontend/ui_plat.c) logs one line per
input frame: total / input+layout / paint microseconds and the toolkit
text layouts made. Prints p50 / p95 / max of the typed frames and the mean
layouts per frame. UI_BENCH_WRAPPED=1 finds the window by class when the
exe is a wrapper such as `perf record -- cctext-ui`. --shots DIR saves a root-window screenshot per fixture
after typing (for before/after visual diffs).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def start_x():
    env = dict(os.environ)
    xvfb = shutil.which("Xvfb")
    if env.get("DISPLAY") or not xvfb:
        return env, None
    for disp in range(121, 160):
        if os.path.exists("/tmp/.X11-unix/X%d" % disp) or \
                os.path.exists("/tmp/.X%d-lock" % disp):
            continue
        p = subprocess.Popen([xvfb, ":%d" % disp, "-screen", "0", "1280x800x24",
                              "-nolisten", "tcp"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists("/tmp/.X11-unix/X%d" % disp):
                env["DISPLAY"] = ":%d" % disp
                return env, p
            time.sleep(0.1)
        p.kill()
    return None, None


def xdo(env, *args):
    return subprocess.run(["xdotool"] + list(args), env=env,
                          capture_output=True, text=True).stdout.split()


def fixtures(tmp):
    code = os.path.join(tmp, "code.c")
    shutil.copy(os.path.join(ROOT, "frontend", "ui_os_win32.c"), code)
    md = os.path.join(tmp, "doc.md")
    with open(md, "w") as f:
        for name in ("mixed_doc.md", "headings.md", "inline.md", "lists_tasks.md"):
            with open(os.path.join(ROOT, "testdata", "rich", "md", name)) as g:
                f.write(g.read())
                f.write("\n")
    long_ = os.path.join(tmp, "long.txt")
    with open(long_, "w") as f:
        word = "alpha beta gamma delta epsilon zeta eta theta iota kappa "
        for i in range(60):
            f.write("%d " % i + word * 40 + "\n")
    table = os.path.join(tmp, "table.md")
    with open(table, "w") as f:
        # A big Rich table whose long cells fit-and-wrap to the pane.
        f.write("# Table\n\n| command | id | notes |\n|---|---|---|\n")
        for i in range(400):
            f.write("| `cmd.%d.run` | **id_%d** | %s |\n" % (
                i, i, "wraps inside its cell when the pane is narrow " * 3))
    # name, path, keys to place the caret before typing
    return [("code", code, ["Down"] * 12 + ["End"]),
            ("markdown", md, ["Down"] * 6 + ["End"]),
            ("longline", long_, ["Down"] * 3),
            ("table", table, ["Down"] * 8 + ["End"]),
            ("table_nw", table, ["ctrl+shift+m"] + ["Down"] * 8 + ["End"])]


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))
    return xs[k]


def run_one(exe, env, tmp, name, path, keys, nchars, delay, shots):
    log = os.path.join(tmp, name + ".frames")
    if os.path.exists(log):
        os.remove(log)
    e = dict(env)
    e.update({"RTX_UI_FRAME_LOG": log, "RTX_SAFE_HOME": os.path.join(tmp, "safe_" + name)})
    e.pop("RTX_UI_SCRIPT", None)
    e.pop("RTX_UI_LOG", None)
    p = subprocess.Popen([exe, "--no-blink", path], env=e,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        win = []
        for _ in range(40):
            time.sleep(0.25)
            win = xdo(e, "search", "--onlyvisible", "--pid", str(p.pid))
            if not win and os.environ.get("UI_BENCH_WRAPPED"):
                # exe is a wrapper (perf record …): the window's pid differs.
                win = xdo(e, "search", "--onlyvisible", "--class", "cctext")
            if win:
                break
        time.sleep(0.8)
        if not win:
            print("%s: window not found" % name)
            return None
        xdo(e, "windowfocus", "--sync", win[0])
        time.sleep(0.3)
        for k in keys:
            xdo(e, "key", "--window", win[0], k)
            time.sleep(0.03)
        time.sleep(0.5)
        with open(log) as f:
            skip = len(f.readlines())
        text = ("the quick brown fox jumps over the lazy dog " * 10)[:nchars]
        xdo(e, "type", "--delay", str(delay), text)
        time.sleep(0.8)
        if shots:
            os.makedirs(shots, exist_ok=True)
            subprocess.run(["import", "-window", "root",
                            os.path.join(shots, name + ".png")], env=e,
                           capture_output=True)
    finally:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    with open(log) as f:
        rows = [l.split() for l in f.readlines()[skip:] if l.startswith("frame ")]
    tot = [float(r[1]) / 1000.0 for r in rows]
    lay = [float(r[2]) / 1000.0 for r in rows]
    pnt = [float(r[3]) / 1000.0 for r in rows]
    nl = [int(r[4]) for r in rows]
    return {"n": len(rows), "p50": pct(tot, 50), "p95": pct(tot, 95),
            "max": max(tot) if tot else 0, "lay50": pct(lay, 50),
            "paint50": pct(pnt, 50),
            "layouts": (sum(nl) / float(len(nl))) if nl else 0}


def main(argv):
    exe = os.path.join(ROOT, "bin", "cctext-ui")
    nchars, delay, shots, only = 60, 60, None, None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--chars":
            nchars = int(argv[i + 1]); i += 2; continue
        if a == "--delay":
            delay = int(argv[i + 1]); i += 2; continue
        if a == "--shots":
            shots = argv[i + 1]; i += 2; continue
        if a == "--only":
            only = argv[i + 1]; i += 2; continue
        exe = a
        i += 1
    exe = os.path.abspath(exe)
    if not shutil.which("xdotool"):
        print("skip: no xdotool")
        return 0
    env, xvfb = start_x()
    if env is None:
        print("skip: no X display")
        return 0
    try:
        with tempfile.TemporaryDirectory(prefix="cctext_type_") as tmp:
            print("%-9s %5s %8s %8s %8s %9s %9s %9s" % (
                "fixture", "n", "p50 ms", "p95 ms", "max ms", "lay50 ms",
                "paint50", "layouts"))
            for name, path, keys in fixtures(tmp):
                if only and name != only:
                    continue
                r = run_one(exe, env, tmp, name, path, keys, nchars, delay, shots)
                if not r:
                    continue
                print("%-9s %5d %8.2f %8.2f %8.2f %9.2f %9.2f %9.1f" % (
                    name, r["n"], r["p50"], r["p95"], r["max"], r["lay50"],
                    r["paint50"], r["layouts"]))
    finally:
        if xvfb:
            xvfb.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

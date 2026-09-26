#!/usr/bin/env python3
"""Caret blink in cctext-ui.

Default (caret layer): the caret is its own layer over the text area. A
blink turn changes only that layer: no relayout, no whole-window paint,
and the one area Draw GTK runs under the layer ("layer-only") returns
before the painter — no text is measured or drawn. The caret's pixels
really toggle (sampled with `import` when present), and a caret moved
over its old frame (hex: two carets, one frame) shows the text under it.

--caret=cell (the painted fallback): a blink repaints only the caret's
row band. Both stop blinking once idle or unfocused, after which the
process does not wake.

    python3 tests/ui_blink_test.py [path/to/cctext-ui]

Needs an X display: $DISPLAY, else a private Xvfb (skipped if neither).
The focus case also needs xdotool. RTX_UI_LOG carries the host's paint
trace ("paint full", "paint blink layer N", "paint blink N rects",
"blink rect x y w h", "relayout ...") and one "draw x y w h kind" line per
area Draw (kind whole / part / layer-only); RTX_BLINK_IDLE_MS shortens the
10 s idle timeout. Exit status is the number of failed checks.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

FAILS = []
IDLE_MS = 1500
WIN_W, WIN_H = 960, 640  # InitWindow in frontend/ui.ccs
CARET_RGB = "FFC850"     # GUI_CARET_C in frontend/gui_draw.ccs


def check(cond, name, detail=""):
    if cond:
        print("ok:   " + name)
    else:
        FAILS.append(name)
        print("FAIL: %s %s" % (name, detail))


def wakeups(pid):
    n = 0
    for tid in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/status" % (pid, tid)) as f:
                for line in f:
                    if line.startswith(("voluntary_ctxt_switches",
                                        "nonvoluntary_ctxt_switches")):
                        n += int(line.split()[1])
        except OSError:
            pass
    return n


def strip_ms(line):
    """Host trace lines lead with a monotonic ms stamp; plat notes do not."""
    head, _, rest = line.partition(" ")
    return rest if head.isdigit() else line


def read_log(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except OSError:
        return []


def start_x():
    """Return (env, xvfb_proc or None), or None when no display is usable."""
    env = dict(os.environ)
    if env.get("DISPLAY"):
        return env, None
    xvfb = shutil.which("Xvfb")
    if not xvfb:
        return None
    for disp in range(91, 120):
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
            if p.poll() is not None:
                break
            time.sleep(0.1)
        p.kill()
        p.wait()
    return None


def launch(exe, env, tmp, name, body, extra_env=None, args=()):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(body)
    log = os.path.join(tmp, name + ".log")
    e = dict(env)
    e.update({"RTX_UI_LOG": log, "RTX_SAFE_HOME": os.path.join(tmp, "safe"),
              "RTX_BLINK_IDLE_MS": str(IDLE_MS)})
    e.pop("RTX_UI_SCRIPT", None)
    if extra_env:
        e.update(extra_env)
    p = subprocess.Popen([exe] + list(args) + [path], env=e,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p, log


def stop(p):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()


def caret_pixels(env, tmp):
    """Pixels of the caret color on screen, or None without `import`."""
    if not shutil.which("import") or not shutil.which("convert"):
        return None
    shot = os.path.join(tmp, "shot.png")
    subprocess.run(["import", "-window", "root", shot], env=env,
                   capture_output=True)
    out = subprocess.run(["convert", shot, "txt:-"], capture_output=True,
                         text=True).stdout
    return out.count(CARET_RGB)


def after_first_blink(lines):
    first = next((i for i, l in enumerate(lines) if "paint blink" in l), None)
    if first is None:
        return []
    return [strip_ms(l) for l in lines[first:]]


def case_blink_layer(exe, env, tmp):
    """Launch counts as input: the caret blinks for IDLE_MS, then stops.
    Every blink is the layer's phase; the text area is not painted."""
    tag = "layer"
    body = b"".join(b"line %d of some text\n" % i for i in range(200))
    p, log = launch(exe, env, tmp, "layer.txt", body)
    lit = []
    try:
        time.sleep(0.9)
        for _ in range(4):  # 4 samples 0.27 s apart: both phases
            lit.append(caret_pixels(env, tmp))
            time.sleep(0.27)
        time.sleep(max(0.0, IDLE_MS / 1000.0 + 0.8 - 0.9 - 4 * 0.27))
        w0 = wakeups(p.pid)
        n0 = len(read_log(log))
        time.sleep(1.5)
        idle_wake = wakeups(p.pid) - w0
        idle_lines = read_log(log)[n0:]
        rest = caret_pixels(env, tmp)
    finally:
        stop(p)
    lines = read_log(log)
    tail = after_first_blink(lines)
    blinks = [l for l in tail if l.startswith("paint blink")]
    draws = [l for l in tail if l.startswith("draw ")]
    painted = [l for l in draws if not l.endswith(" layer-only")]
    other = [l for l in tail if not l.startswith(("paint blink", "draw ",
                                                  "dirty=", "focus", "panes="))]
    check(any(l.endswith("paint full") for l in lines),
          "%s: first frame paints the window" % tag)
    check(len(blinks) >= 2 and all(l.startswith("paint blink layer") for l in blinks),
          "%s: caret blinks on the layer" % tag, repr(blinks[:3]))
    check(not other, "%s: a blink neither lays out nor paints the window" % tag,
          repr(other[:4]))
    check(not painted, "%s: a blink paints no text (no area Draw reaches the painter)" % tag,
          repr(painted[:4]))
    check(len(draws) <= len(blinks),
          "layer: at most one (skipped) area Draw per blink",
          "%d Draws in %d blinks" % (len(draws), len(blinks)))
    check(blinks and blinks[-1].endswith(" 1"), "%s: blink stops with the caret shown" % tag,
          repr(blinks[-1:]))
    check(not idle_lines, "%s: idle: no paint after the timeout" % tag,
          repr(idle_lines[:3]))
    check(idle_wake <= 1, "%s: idle: the host does not wake" % tag,
          "%d wakeups in 1.5s" % idle_wake)
    if None in lit or rest is None:
        print("skip: %s caret pixels (no ImageMagick import)" % tag)
        return
    check(max(lit) > 0 and min(lit) == 0,
          "%s: the caret's pixels toggle (on and off seen)" % tag, repr(lit))
    check(rest > 0, "%s: idle caret is shown" % tag, repr(rest))


def case_hex_layer(exe, env, tmp):
    """Hex: two carets (hex nibble, text column) on one layer frame. After
    the caret moves over its old frame, the text the layer shows while off
    equals a --caret=cell repaint of the same state (no stale pixels)."""
    if not shutil.which("xdotool") or not shutil.which("import"):
        print("skip: hex layer (no xdotool / import)")
        return
    body = bytes(range(0x41, 0x5b)) + b"0123456789abcdefghijklmnopqrstuvwxyz\n"
    shots = {}
    for mode, args in (("layer", ["--hex", "--no-blink"]),
                       ("cell", ["--hex", "--no-blink", "--caret=cell"])):
        p, _ = launch(exe, env, tmp, "hex_%s.bin" % mode, body,
                      {"RTX_SAFE_HOME": os.path.join(tmp, "safe_" + mode)},
                      args=args)
        try:
            time.sleep(1.2)
            win = xdo(env, "search", "--onlyvisible", "--pid", str(p.pid))
            if not win:
                print("skip: hex layer (window not found)")
                return
            for k in ("Right", "Right", "Right", "Left"):
                xdo(env, "key", "--window", win[0], k)
                time.sleep(0.15)
            time.sleep(0.5)
            shot = os.path.join(tmp, "hex_%s.png" % mode)
            subprocess.run(["import", "-window", "root", shot], env=env,
                           capture_output=True)
            shots[mode] = shot
        finally:
            stop(p)
    # The row with the carets, less the caret bars (a painted bar sits under
    # its glyph, the layer's over it: a few pixels either side differ).
    # Every other pixel must match the painted-caret frame.
    width = 900
    rows = []
    for mode in ("layer", "cell"):
        out = subprocess.run(["convert", shots[mode], "-crop",
                              "%dx18+0+33" % width, "txt:-"],
                             capture_output=True, text=True).stdout
        rows.append([l.split()[2].upper() for l in out.splitlines()[1:]])
    lay, cell = rows
    near = set()
    for px in (lay, cell):
        for i, a in enumerate(px):
            if CARET_RGB in a:
                near.update(range(i % width - 3, i % width + 4))
    bad = sum(1 for i, (a, b) in enumerate(zip(lay, cell))
              if a != b and i % width not in near)
    carets = sum(1 for a in lay if CARET_RGB in a)
    check(carets >= 2 * 18, "hex layer: both caret bars on the layer",
          "%d caret pixels" % carets)
    check(bad == 0, "hex layer: text under a moved frame matches a painted frame",
          "%d pixels differ" % bad)


def case_blink_cell(exe, env, tmp):
    """--caret=cell: the painted caret. A blink repaints the caret's row
    band only (no relayout, no whole-window paint)."""
    body = b"".join(b"line %d of some text\n" % i for i in range(200))
    p, log = launch(exe, env, tmp, "blink.txt", body, args=["--caret=cell"])
    try:
        time.sleep(IDLE_MS / 1000.0 + 1.5)  # blink window + one phase + slack
        w0 = wakeups(p.pid)
        n0 = len(read_log(log))
        time.sleep(1.5)
        idle_wake = wakeups(p.pid) - w0
        idle_lines = read_log(log)[n0:]
    finally:
        stop(p)
    lines = read_log(log)
    check(any(l.endswith("paint full") for l in lines),
          "cell: first frame paints the window")
    tail = after_first_blink(lines)
    blinks = [l for l in tail if l.startswith("paint blink")]
    rects = [l for l in tail if l.startswith("blink rect")]
    other = [l for l in tail if not l.startswith(("paint blink", "blink rect",
                                                  "dirty=", "focus", "draw ", "panes="))]
    check(len(blinks) >= 2, "cell: caret blinks", "%d blink turns" % len(blinks))
    check(not other, "cell: a blink turn neither lays out nor paints the window",
          repr(other[:4]))
    check(all(l == "paint blink 1 rects" for l in blinks),
          "cell: one rect per blink (one caret)", repr(blinks[:3]))
    geo = [tuple(int(v) for v in re.findall(r"-?\d+", l)[:4]) for l in rects]
    check(bool(geo) and all(0 < h <= 32 and w < WIN_W and 0 <= y < WIN_H - 26
                            for x, y, w, h in geo),
          "cell: blink rect is the caret's row, not the window", repr(geo[:3]))
    check(len(set(geo)) == 1, "cell: caret did not move: same rect every blink",
          repr(sorted(set(geo))[:3]))
    # Stops on a visible phase: an even number of toggles.
    check(len(blinks) % 2 == 0, "cell: blink stops with the caret shown",
          "%d toggles" % len(blinks))
    check(not idle_lines, "cell: idle: no paint after the timeout",
          repr(idle_lines[:3]))
    check(idle_wake <= 1, "cell: idle: the host does not wake",
          "%d wakeups in 1.5s" % idle_wake)


def xdo(env, *args):
    return subprocess.run(["xdotool"] + list(args), env=env,
                          capture_output=True, text=True).stdout.split()


def case_blink_unfocused(exe, env, tmp):
    """Another window takes focus: no blink, no wakeups; focus back blinks."""
    if not shutil.which("xdotool"):
        print("skip: unfocused blink (no xdotool)")
        return
    p, log = launch(exe, env, tmp, "a.txt", b"alpha\n", {"RTX_BLINK_IDLE_MS": "60000"})
    q, _ = launch(exe, env, tmp, "b.txt", b"beta\n")
    try:
        time.sleep(2.5)
        wa = xdo(env, "search", "--onlyvisible", "--pid", str(p.pid))
        wb = xdo(env, "search", "--onlyvisible", "--pid", str(q.pid))
        if not wa or not wb:
            print("skip: unfocused blink (windows not found)")
            return
        xdo(env, "windowfocus", wa[0])
        time.sleep(0.5)
        xdo(env, "windowfocus", wb[0])
        time.sleep(0.8)
        n0 = len(read_log(log))
        w0 = wakeups(p.pid)
        time.sleep(1.5)
        away = read_log(log)[n0:]
        away_wake = wakeups(p.pid) - w0
        xdo(env, "windowfocus", wa[0])
        time.sleep(1.4)
        back = read_log(log)[n0 + len(away):]
    finally:
        stop(q)
        stop(p)
    lines = read_log(log)
    if not any(l.endswith("focus 0") for l in lines):
        print("skip: unfocused blink (no focus events from the X server)")
        return
    check(not away, "unfocused: no paint", repr(away[:3]))
    check(away_wake <= 1, "unfocused: the host does not wake",
          "%d wakeups in 1.5s" % away_wake)
    check(any(l.endswith("focus 1") for l in back) and
          sum(1 for l in back if "paint blink" in l) >= 2,
          "focus back: blinking again", repr(back[:4]))


def case_tabs_panes(exe, env, tmp):
    """Buffers and panes in cctext-ui, driven by RTX_UI_SCRIPT: Cmd-\\ and
    Cmd-- split right and down, Cmd-W on an unsaved buffer asks (the
    script answers Don't Save) and closes it; the panes stay. The host's
    "panes=N bufs=M focus=F ask_close=A" log line records each step."""
    names = []
    for n in ("tp_a.txt", "tp_b.txt"):
        path = os.path.join(tmp, n)
        with open(path, "wb") as f:
            f.write(b"".join(b"%s %d\n" % (n.encode(), i) for i in range(40)))
        names.append(path)
    script = os.path.join(tmp, "tabs.script")
    with open(script, "w") as f:
        f.write("wait 20\ncmd \\\nwait 5\ncmd -\nwait 5\nkey X\nwait 5\n"
                "cmd w\nwait 5\ndlg2\nwait 10\n")
    p, log = launch(exe, env, tmp, "tp_c.txt", b"third\n",
                    extra_env={"RTX_UI_SCRIPT": script}, args=names)
    try:
        time.sleep(4.0)
        alive = p.poll() is None
    finally:
        stop(p)
    lines = [l for l in read_log(log) if l.startswith("panes=")]
    check(alive, "tabs: closing a buffer does not quit")
    check(any(l.startswith("panes=2 bufs=3") for l in lines),
          "tabs: three files open in two panes", repr(lines[:2]))
    check(any(l.startswith("panes=3 bufs=3") for l in lines) and
          any(l.startswith("panes=4 bufs=3") for l in lines),
          "tabs: Cmd-\\ and Cmd-- split", repr(lines))
    check(any(l.endswith("ask_close=1") for l in lines),
          "tabs: Cmd-W on an unsaved buffer asks", repr(lines))
    check(bool(lines) and lines[-1].startswith("panes=4 bufs=2") and
          lines[-1].endswith("ask_close=0"),
          "tabs: Don't Save closes the buffer, the panes stay", repr(lines[-2:]))


MD_CHROME = (132, 138, 152)   # gui_md_stroke in frontend/gui_draw.ccs
MD_FILLS = ((42, 46, 60), (32, 34, 42))  # header / body band


def shot_pixels(env, tmp, name):
    """Root-window pixels as {(x, y): (r, g, b)} rows, or None."""
    if not shutil.which("import") or not shutil.which("convert"):
        return None
    shot = os.path.join(tmp, name)
    subprocess.run(["import", "-window", "root", shot], env=env,
                   capture_output=True)
    out = subprocess.run(["convert", shot, "-depth", "8", "txt:-"],
                         capture_output=True, text=True).stdout
    px = {}
    for line in out.splitlines():
        m = re.match(r"(\d+),(\d+): \((\d+),(\d+),(\d+)", line)
        if m:
            x, y, r, g, b = (int(v) for v in m.groups())
            px[(x, y)] = (r, g, b)
    return px


def table_right_edge(px, y0, y1, x_hi):
    """Rightmost table rule x over rows [y0, y1) and whether table fill
    runs on past it (a clipped, overflowing table) — (x, runs_on)."""
    rules = {}
    rows = 0
    for y in range(y0, y1):
        seen = False
        for x in range(0, x_hi):
            if px.get((x, y)) == MD_CHROME:
                rules[x] = rules.get(x, 0) + 1
                seen = True
        rows += seen
    if not rows:
        return None, False
    cols = [x for x, n in rules.items() if n * 2 >= rows]
    if not cols:
        return None, False
    xr = max(cols)
    runs_on = 0
    for y in range(y0, y1):
        if px.get((xr + 3, y)) in MD_FILLS:
            runs_on += 1
    return xr, runs_on * 4 >= (y1 - y0)


FIT_W, FIT_H = 1260, 760  # the window is resized to this first (a refit)


def case_md_table_fit(exe, env, tmp):
    """README.md's command table in Rich view, soft wrap off: it is fitted
    to the window (cells wrap), so its right border is drawn inside the
    text area and no table fill runs on under the clip edge. The window
    is resized first, so the fit is the resized pane's."""
    if not shutil.which("xdotool"):
        print("skip: table fit (no xdotool)")
        return
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "README.md"), "rb") as f:
        body = f.read()
    lines = body.split(b"\n")
    hdr = next((i for i, l in enumerate(lines)
                if l.startswith(b"| Command | id |")), None)
    if hdr is None:
        check(False, "table fit: README has the command table")
        return
    p, log = launch(exe, env, tmp, "README.md", body)
    px = None
    try:
        time.sleep(2.5)
        win = xdo(env, "search", "--onlyvisible", "--pid", str(p.pid))
        if not win:
            print("skip: table fit (window not found)")
            return
        xdo(env, "windowsize", "--sync", win[0], str(FIT_W), str(FIT_H))
        time.sleep(0.8)
        xdo(env, "windowfocus", "--sync", win[0])
        time.sleep(0.3)
        # Wrap off (Ctrl-Shift-M), then Go to Line: the header near the top.
        for k in ["ctrl+shift+m", "ctrl+g"] + list(str(hdr)) + ["Return"]:
            xdo(env, "key", "--window", win[0], k)
            time.sleep(0.08)
        time.sleep(1.0)
        px = shot_pixels(env, tmp, "fit.png")
    finally:
        stop(p)
    if px is None:
        print("skip: table fit (no import / convert)")
        return
    # Pane text area: below the menu, above the status bar, left of the rail.
    xr, runs_on = table_right_edge(px, 60, FIT_H - 60, FIT_W)
    check(xr is not None, "table fit: the command table is on screen")
    if xr is None:
        return
    check(not runs_on and xr < FIT_W - 4,
          "table fit: README command table fits the window (wrap off)",
          "right rule x=%d runs_on=%s" % (xr, runs_on))


def main(argv):
    exe = os.path.abspath(argv[1] if len(argv) > 1 else "bin/cctext-ui")
    if not os.path.exists(exe):
        print("skip: %s not built" % exe)
        return 0
    if not os.path.exists("/proc/self/status"):
        print("skip: no /proc")
        return 0
    got = start_x()
    if not got:
        print("skip: no X display (set DISPLAY or install Xvfb)")
        return 0
    env, xvfb = got
    try:
        with tempfile.TemporaryDirectory(prefix="cctext_blink_") as tmp:
            case_blink_layer(exe, env, tmp)
            case_hex_layer(exe, env, tmp)
            case_blink_cell(exe, env, tmp)
            case_blink_unfocused(exe, env, tmp)
            case_tabs_panes(exe, env, tmp)
            case_md_table_fit(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(FAILS))
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

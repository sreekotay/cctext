#!/usr/bin/env python3
"""cctext-ui presentation mode under Xvfb (docs/slides.md).

Opens testdata/slides/demo.md, presses Shift-F5 with xdotool and checks
coarse properties of screenshots, never exact pixels:

- the slide is letterboxed at 16:9 (dark bars above and below, the
  slide's white background filling the width), the editor's status bar
  is gone and the title's heading colour is on screen;
- a build step adds ink (a fragment appears), Left takes it away again;
- slide 5's `_backgroundColor: #1e1e2e` fills the slide;
- a transition paints at the frame clock (many paints within its
  duration, RTX_UI_LOG) and then stops: a still slide paints nothing,
  barely wakes and uses no CPU;
- Esc brings the editor back with "slide N/11" in the status bar.

    python3 tests/ui_present_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool and ImageMagick (`import`, `convert`);
skips otherwise. Exit status is the number of failed checks.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402  (start_x, launch, stop, wakeups, xdo)

FAILS = []
DECK = os.path.join(HERE, "..", "testdata", "slides", "demo.md")


def check(cond, name, detail=""):
    if cond:
        print("ok:   %s%s" % (name, ("  (" + detail + ")") if detail else ""))
    else:
        FAILS.append(name)
        print("FAIL: %s %s" % (name, detail))


def cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])


class Shot:
    """RGB pixels of the window's client area."""

    def __init__(self, env, tmp, win, name):
        self.ok = False
        geo = subprocess.run(["xdotool", "getwindowgeometry", "--shell", win], env=env,
                             capture_output=True, text=True).stdout
        vals = dict(l.split("=", 1) for l in geo.split() if "=" in l)
        x, y = int(vals.get("X", 0)), int(vals.get("Y", 0))
        self.w, self.h = int(vals.get("WIDTH", 0)), int(vals.get("HEIGHT", 0))
        if self.w <= 0 or self.h <= 0:
            return
        png = os.path.join(tmp, name + ".png")
        subprocess.run(["import", "-window", "root", png], env=env, capture_output=True)
        raw = subprocess.run(["convert", png, "-crop", "%dx%d+%d+%d" % (self.w, self.h, x, y),
                              "+repage", "-depth", "8", "rgb:-"], capture_output=True).stdout
        if len(raw) < self.w * self.h * 3:
            return
        self.px = raw
        self.ok = True

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return self.px[i], self.px[i + 1], self.px[i + 2]

    def count(self, pred, y0=0, y1=None, step=2):
        y1 = self.h if y1 is None else y1
        n = tot = 0
        for y in range(y0, y1, step):
            for x in range(0, self.w, step):
                tot += 1
                if pred(self.at(x, y)):
                    n += 1
        return n, tot

    def rows_where(self, pred, frac=0.9):
        """Rows where at least `frac` of the (sampled) pixels match."""
        out = []
        for y in range(self.h):
            n = sum(1 for x in range(0, self.w, 4) if pred(self.at(x, y)))
            if n >= frac * (self.w // 4):
                out.append(y)
        return out


def near(c, rgb, tol=10):
    return all(abs(a - b) <= tol for a, b in zip(c, rgb))


def white(c):
    return near(c, (255, 255, 255), 3)


def dark_bar(c):
    return near(c, (12, 12, 14), 6)


def ink(c):
    return max(c) < 150  # text on the white slide


def paints(lines):
    return [int(l.split()[0]) for l in lines if l.endswith("paint full") and
            l.split()[0].isdigit()]


def main(argv):
    exe = os.path.abspath(argv[1] if len(argv) > 1 else "bin/cctext-ui")
    if not os.path.exists(exe):
        print("skip: %s not built" % exe)
        return 0
    for tool in ("xdotool", "import", "convert"):
        if not shutil.which(tool):
            print("skip: no %s" % tool)
            return 0
    got = U.start_x()
    if not got:
        print("skip: no X display (set DISPLAY or install Xvfb)")
        return 0
    env, xvfb = got
    try:
        with tempfile.TemporaryDirectory(prefix="cctext_present_") as tmp:
            with open(DECK, "rb") as f:
                body = f.read()
            p, log = U.launch(exe, env, tmp, "deck.md", body, {"RTX_BLINK_IDLE_MS": "300"})
            try:
                run(env, tmp, p, log)
            finally:
                U.stop(p)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(FAILS))
    return len(FAILS)


def run(env, tmp, p, log):
    win = []
    for _ in range(60):
        time.sleep(0.25)
        win = U.xdo(env, "search", "--onlyvisible", "--pid", str(p.pid))
        if win or p.poll() is not None:
            break
    if not win:
        print("skip: window not found")
        return
    win = win[0]

    def key(*keys, settle=0.5):
        for k in keys:
            U.xdo(env, "key", "--window", win, k)
            time.sleep(0.05)
        time.sleep(settle)

    time.sleep(0.6)
    edit = Shot(env, tmp, win, "edit")
    key("shift+F5", settle=0.8)
    s1 = Shot(env, tmp, win, "title")
    if not (edit.ok and s1.ok):
        print("skip: no screenshot")
        return
    # Letterbox: dark bars above and below, the white slide between; its
    # height is 9/16 of the width (the menu bar sits above the area).
    whites = s1.rows_where(white, 0.97)
    bars = s1.rows_where(dark_bar, 0.97)
    slide_h = (max(whites) - min(whites) + 1) if whites else 0
    check(bool(whites) and abs(slide_h - s1.w * 9 / 16) <= 0.12 * s1.w * 9 / 16,
          "title: a white 16:9 slide fills the width",
          "slide rows %s..%s (h %d, w %d)" % (min(whites) if whites else None,
                                              max(whites) if whites else None,
                                              slide_h, s1.w))
    check(any(y < min(whites or [0]) for y in bars) and any(y > max(whites or [0]) for y in bars),
          "title: letterbox bars above and below", "%d bar rows" % len(bars))
    status_rgb = (36, 38, 46)
    check(sum(1 for x in range(0, edit.w, 4) if near(edit.at(x, edit.h - 10), status_rgb, 4)) >
          edit.w // 8, "editor: status bar before presenting")
    check(sum(1 for x in range(0, s1.w, 4) if near(s1.at(x, s1.h - 10), status_rgb, 4)) == 0,
          "title: no editor chrome while presenting")
    heading, _ = s1.count(lambda c: near(c, (0x1f, 0x3a, 0x5f), 40) and c[2] > c[0] + 30)
    check(heading > 50, "title: heading colour on screen", "%d px" % heading)

    # Build steps: slide 2, step 0 then step 1 (more ink), Left (less).
    key("Right", settle=0.8)
    a, _ = Shot(env, tmp, win, "s2").count(ink)
    key("space", settle=0.6)
    b, _ = Shot(env, tmp, win, "s2a").count(ink)
    key("Left", settle=0.6)
    c, _ = Shot(env, tmp, win, "s2b").count(ink)
    check(b > a + 20, "step: a build step adds ink", "%d -> %d px" % (a, b))
    check(abs(c - a) <= 10, "step: Left takes the step away", "%d vs %d px" % (c, a))

    # Slide 5: spot backgroundColor.
    key("5", "Return", settle=0.9)
    s5 = Shot(env, tmp, win, "s5")
    bg, tot = s5.count(lambda c: near(c, (0x1e, 0x1e, 0x2e), 3))
    check(bg > tot * 0.4, "slide 5: _backgroundColor #1e1e2e fills the slide",
          "%d of %d px" % (bg, tot))

    # Transition frames, then idle: slide 3 -> 4 is a 400 ms push after
    # three steps; count paints in the 700 ms after the push.
    key("3", "Return", settle=0.9)
    key("Right", "Right", "Right", settle=0.5)
    n0 = len(U.read_log(log))
    U.xdo(env, "key", "--window", win, "Right")
    time.sleep(0.7)
    frames = paints(U.read_log(log)[n0:])
    check(len(frames) >= 8, "push: painted at the frame clock during the transition",
          "%d paints in 0.7 s" % len(frames))
    if len(frames) >= 3:
        gaps = [b - a for a, b in zip(frames, frames[1:])]
        gaps.sort()
        check(gaps[len(gaps) // 2] <= 40, "push: frames about 16 ms apart",
              "median gap %d ms" % gaps[len(gaps) // 2])
    time.sleep(0.3)
    n1 = len(U.read_log(log))
    w0 = U.wakeups(p.pid)
    c0 = cpu_ticks(p.pid)
    time.sleep(2.0)
    idle = paints(U.read_log(log)[n1:])
    wk = U.wakeups(p.pid) - w0
    cpu = cpu_ticks(p.pid) - c0
    check(not idle, "idle: a still slide paints nothing", "%d paints" % len(idle))
    check(wk <= 3, "idle: the host does not wake", "%d wakeups in 2 s" % wk)
    check(cpu <= 2, "idle: no CPU on a still slide", "%d ticks in 2 s" % cpu)
    s4 = Shot(env, tmp, win, "s4")
    code, _ = s4.count(lambda c: near(c, (0x1f, 0x24, 0x30), 3))
    check(code > 500, "slide 4: the code band is drawn", "%d px" % code)

    # Esc: the editor again, status bar names the slide.
    key("Escape", settle=0.8)
    back = Shot(env, tmp, win, "back")
    check(sum(1 for x in range(0, back.w, 4) if near(back.at(x, back.h - 10), status_rgb, 4)) >
          back.w // 8, "Esc: the editor's status bar is back")
    check(p.poll() is None, "present: the editor is still running")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

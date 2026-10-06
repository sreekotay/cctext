#!/usr/bin/env python3
"""Wheel and trackpad scrolling in cctext-ui (Xvfb), driven by RTX_UI_SCRIPT.

- Sub-notch deltas (`wheel -0.25`: a trackpad's fraction of a notch) paint
  the frame they arrive in. They used to scroll the camera without counting
  as input, so nothing painted until a whole notch or a caret blink, and
  the view then jumped by everything since: the GUI's scroll stutter.
- Precise deltas (`wheelpx -5`: a macOS trackpad's pixels) scroll a text
  pane by exactly that many pixels (they were 16 px to a notch of 3 lines,
  ~3.4x the finger's motion).
- A mostly-vertical swipe that drifts sideways (`wheelpx -14 11`: under a
  vertical notch, over a horizontal one) stays vertical: the horizontal
  notch used to take the frame and drop its vertical motion.
- A mouse wheel notch (`wheel -1`) is still GUI_WHEEL_LINES (3) lines.

The host logs `view top=T wrap=W dy=D unit=U` each paint the camera moved
(RTX_UI_LOG); a plain-text file of short lines has one row per line, so
the distance scrolled is T * U + D.
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402

FAILS = []


def check(cond, what, detail=""):
    if cond:
        print("ok:   %s" % what)
    else:
        print("FAIL: %s  %s" % (what, detail))
        FAILS.append(what)


def views(log):
    out = []
    for l in U.read_log(log):
        parts = l.split()
        if len(parts) >= 6 and parts[1] == "view":
            kv = dict(p.split("=", 1) for p in parts[2:])
            out.append((int(kv["top"]), int(kv["wrap"]), float(kv["dy"]), float(kv["unit"])))
    return out


def run(exe, env, tmp, tag, lines, frames):
    body = b"".join(b"line %d\n" % i for i in range(4000))
    script = os.path.join(tmp, tag + ".script")
    with open(script, "w") as f:
        f.write("wait 40\n")
        for _ in range(frames):
            f.write(lines + "\n")
        f.write("wait 20\n")
    p, log = U.launch(exe, env, tmp, tag + ".txt", body,
                      extra_env={"RTX_UI_SCRIPT": script})
    try:
        time.sleep(1.0 + frames * 0.02 + 2.5)
    finally:
        U.stop(p)
    log_lines = U.read_log(log)
    v = views(log)
    paints = sum(1 for l in log_lines if l.endswith("paint full"))
    moved = None
    if v:
        top, wrap, dy, unit = v[-1]
        moved = (top + wrap) * unit + dy
    return v, paints, moved


def main(argv):
    exe = os.path.abspath(argv[1] if len(argv) > 1 else "bin/cctext-ui")
    if not os.path.exists(exe):
        print("skip: %s not built" % exe)
        return 0
    got = U.start_x()
    if not got:
        print("skip: no X display (set DISPLAY or install Xvfb)")
        return 0
    env, xvfb = got
    try:
        with tempfile.TemporaryDirectory(prefix="cctext_wheel_") as tmp:
            n = 40
            v, paints, moved = run(exe, env, tmp, "fine", "wheel -0.25", n)
            check(len(v) >= n - 2,
                  "sub-notch: every fractional delta paints a moved view",
                  "%d view paints for %d frames (%d paints)" % (len(v), n, paints))
            unit = v[-1][3] if v else 0
            want = n * 0.25 * 3 * unit
            check(moved is not None and abs(moved - want) < 1.0,
                  "sub-notch: a quarter notch is a quarter of 3 lines",
                  "moved %s want %.1f" % (moved, want))
            if v:
                steps = [(t + w) * u + d for t, w, d, u in v]
                deltas = [b - a for a, b in zip(steps, steps[1:])]
                check(deltas and max(deltas) <= 0.25 * 3 * unit + 0.5,
                      "sub-notch: no jump bigger than one frame's delta",
                      "largest step %.1f px" % (max(deltas) if deltas else -1))

            v, paints, moved = run(exe, env, tmp, "px", "wheelpx -5", n)
            check(moved is not None and abs(moved - 5 * n) < 0.5,
                  "precise: pixels scroll 1:1", "moved %s want %d" % (moved, 5 * n))
            check(len(v) >= n - 2, "precise: every delta paints",
                  "%d view paints for %d frames" % (len(v), n))

            m = 20
            v, paints, moved = run(exe, env, tmp, "diag", "wheelpx -14 11", m)
            check(moved is not None and abs(moved - 14 * m) < 0.5,
                  "diagonal swipe: mostly vertical stays vertical",
                  "moved %s want %d" % (moved, 14 * m))

            k = 10
            v, paints, moved = run(exe, env, tmp, "notch", "wheel -1", k)
            unit = v[-1][3] if v else 0
            check(moved is not None and abs(moved - 3 * k * unit) < 0.5,
                  "mouse wheel: a notch is 3 lines",
                  "moved %s want %.1f" % (moved, 3 * k * unit))
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(FAILS))
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

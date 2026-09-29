#!/usr/bin/env python3
"""cctext-ui: the wheel scrolls a picture through the pane's top edge.

A Markdown file with a few lines of text, then a picture row, then more
text. Each wheel notch moves the picture's bottom edge up by the same
number of pixels (GUI_WHEEL_LINES lines), including the notches while
the picture is partly above the pane: it slides out, it does not vanish
in one notch (the camera rests inside the picture's row).

    python3 tests/ui_scroll_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool and ImageMagick; skips otherwise. Exit
status is the number of failed checks.
"""
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402
import ui_present_test as P  # noqa: E402
import ui_img_test as I  # noqa: E402

FAILS = []
RED = (220, 30, 30)
GREEN = (30, 200, 40)
BLUE = (40, 60, 220)


def check(cond, name, detail=""):
    if cond:
        print("ok:   %s%s" % (name, ("  (" + detail + ")") if detail else ""))
    else:
        FAILS.append(name)
        print("FAIL: %s %s" % (name, detail))


def pic_rows(shot):
    """First and last screen row holding the picture's colours, or None."""
    ys = []
    for y in range(0, shot.h):
        hits = 0
        for x in range(0, shot.w, 4):
            c = shot.at(x, y)
            if P.near(c, RED, 14) or P.near(c, GREEN, 14) or P.near(c, BLUE, 14):
                hits += 1
                if hits > 10:
                    ys.append(y)
                    break
    return (ys[0], ys[-1]) if ys else None


def case_wheel_picture(exe, env, tmp):
    d = I.proj(tmp, "scroll")
    body = b"# Scroll\n\none\n\ntwo\n\n![big one](big.png)\n\n" + b"".join(
        b"line %d after the picture\n\n" % i for i in range(80))
    p, log = I.launch_in(exe, env, d, "doc.md", body)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: wheel picture (no window)")
            return
        time.sleep(1.5)
        spans = []
        for n in range(24):
            s = P.Shot(env, tmp, win, "sc_%02d" % n)
            if not s.ok:
                print("skip: wheel picture (no screenshot)")
                return
            spans.append(pic_rows(s))
            U.xdo(env, "mousemove", "--window", win, "200", "200")
            U.xdo(env, "click", "5")
            time.sleep(0.35)
        print("picture rows per notch:", spans)
        shown = [sp for sp in spans if sp]
        bottoms = [sp[1] for sp in shown]
        # How far each notch moved it: by its bottom edge, except while the
        # pane's bottom cuts it off (that edge is the pane's, not its own);
        # then by its top edge.
        clip = max(bottoms) if bottoms else 0
        steps = []
        for a, b in zip(shown, shown[1:]):
            steps.append(a[1] - b[1] if a[1] != clip else a[0] - b[0])
        check(len(bottoms) >= 4, "wheel picture: on screen for several notches",
              "%d shots" % len(bottoms))
        # Cut at the top: its top edge stays at the pane's top while the
        # bottom keeps moving, so it shows shorter than it is.
        full = spans[0][1] - spans[0][0] if spans[0] else 0
        cut = [sp for sp in spans if sp and sp[1] - sp[0] < full - 10]
        check(len(cut) >= 3, "wheel picture: slides out through the pane's top",
              "%d shots with the picture cut at the top" % len(cut))
        # Every notch is GUI_WHEEL_LINES lines, through the picture too.
        big = [st for st in steps if st > 0]
        if big:
            lo, hi = min(big), max(big)
            check(hi <= lo + 4, "wheel picture: every notch moves it the same distance",
                  "steps %s" % steps)
    finally:
        U.stop(p)


def main(argv):
    exe = argv[1] if len(argv) > 1 else os.path.join(HERE, "..", "bin", "cctext-ui")
    if not os.path.exists(exe) or not shutil.which("xdotool") or not shutil.which("import"):
        print("skip: needs cctext-ui, xdotool, ImageMagick")
        return 0
    started = U.start_x()
    if not started:
        print("skip: no X")
        return 0
    env, xproc = started
    tmp = tempfile.mkdtemp(prefix="cctext_scroll_")
    try:
        case_wheel_picture(exe, env, tmp)
    finally:
        if xproc:
            xproc.terminate()
        if not os.environ.get("KEEP_SHOTS"):
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("shots in", tmp)
    print("%d failed" % len(FAILS))
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

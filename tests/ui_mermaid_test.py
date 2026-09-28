#!/usr/bin/env python3
"""cctext-ui Mermaid diagrams under Xvfb (docs/images.md, "Mermaid").

Coarse properties of screenshots, as tests/ui_svg_test.py:

- a Markdown file with a ```mermaid fence: the diagram is on screen (the
  dark theme's node fill, drawn by cctext-render's Mermaid slot), its
  fence lines are hidden, and it stays put while the window idles;
- the caret into the fence shows the source with the diagram under it;
  a typed edit keeps the old diagram up, dimmed, until the new one lands
  (the stale display), then shows the new one;
- one helper per slot: an SVG image and a diagram are two processes;
  both exit with the editor;
- idle after the diagram settles: no paints and no CPU on the UI thread
  (the helpers sit blocked in read());
- a Marp slide with a fence shows the diagram in the slide's colours.

The Markdown screenshot is kept as testdata/generated/ui_mermaid_markdown.png.

    python3 tests/ui_mermaid_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool, ImageMagick and bin/cctext-render
beside cctext-ui; skips otherwise. Exit status is the number of failures.
"""
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402
import ui_present_test as P  # noqa: E402
import ui_img_test as I  # noqa: E402
import ui_svg_test as S  # noqa: E402

FLOW = (b"flowchart LR\n  A[Start] --> B{Is it ok?}\n  B -->|yes| C[Ship]\n"
        b"  B -->|no| D[Fix]\n  D --> B\n")


def fill_rgb():
    """The node fill the editor's dark theme gives mermaid: bg (24, 24, 28)
    mixed 22 % toward the accent (110, 150, 230) (core/img.ccs img_mm_opts)."""
    bg, ac = (24, 24, 28), (110, 150, 230)
    return tuple((b * 78 + a * 22 + 50) // 100 for b, a in zip(bg, ac))


FILL = fill_rgb()


def fill_px(shot, rgb=FILL, tol=10):
    return I.count(shot, rgb, tol=tol)


def case_markdown(exe, env, tmp):
    d = S.proj(tmp, "mm")
    body = (b"# Mermaid in Markdown\n\nA diagram drawn by the sandboxed renderer:\n\n```mermaid\n" +
            FLOW + b"```\n\nText after the diagram.\n\n![pipeline](pipe.svg)\n\nend\n")
    p, log = I.launch_in(exe, env, d, "doc.md", body)
    kids = []
    try:
        win = I.window(env, p)
        if not win:
            print("skip: markdown (no window)")
            return
        time.sleep(3.0)
        a = P.Shot(env, tmp, win, "mm_md_a")
        time.sleep(1.0)
        b = P.Shot(env, tmp, win, "mm_md_b")
        if not (a.ok and b.ok):
            print("skip: markdown (no screenshot)")
            return
        fa = fill_px(a)
        I.check(fa > 1500, "mermaid markdown: the diagram is on screen", "%d fill px" % fa)
        ta, tb = I.top_of(a, FILL, tol=10), I.top_of(b, FILL, tol=10)
        I.check(ta is not None and ta == tb and fill_px(b) == fa,
                "mermaid markdown: the diagram stays put", "%s -> %s" % (ta, tb))
        S.save_crop(env, win, tmp, os.path.join(ROOT, "testdata", "generated",
                                                "ui_mermaid_markdown.png"))
        print("      screenshot: testdata/generated/ui_mermaid_markdown.png")
        kids = S.helpers_of(p.pid)
        I.check(len(kids) == 2, "mermaid markdown: two helpers (SVG and Mermaid slots)",
                "%s" % kids)
        # Idle once the diagram settled: no thread of cctext-ui wakes (the
        # helpers are other processes, blocked in read()).
        I.check_idle("mermaid markdown", p.pid)
        # Caret into the fence: the source shows, the diagram under it.
        I.key(env, win, "ctrl+Home", settle=0.3)
        for _ in range(5):
            I.key(env, win, "Down", settle=0.15)
        time.sleep(1.0)
        c = P.Shot(env, tmp, win, "mm_md_c")
        tc = None
        if c.ok:
            # The fence's six lines lay out above the diagram: it moves down.
            tc = I.top_of(c, FILL, tol=10)
            I.check(ta is not None and tc is not None and tc > ta + 90 and fill_px(c) > 1500,
                    "mermaid markdown: the caret shows the fence's source, the diagram under it",
                    "diagram top %s -> %s, %d fill px" % (ta, tc, fill_px(c)))
        # Type into the fence: the old diagram stays (dimmed) at once.
        I.key(env, win, "Down", "End", "Return", settle=0.05)
        U.xdo(env, "type", "--window", win, "--delay", "20", "B --> E[Extra step]")
        s = P.Shot(env, tmp, win, "mm_md_stale")
        if s.ok and c.ok and tc is not None:
            # Dimmed: the diagram's box (one line lower now) still holds
            # its drawing, at a fraction of its colour.
            bg = s.at(s.w - 40, 60)
            ink = sum(1 for y in range(tc + 20, tc + 170, 2) for x in range(60, 700, 2)
                      if sum(abs(a - b) for a, b in zip(s.at(x, y), bg)) > 12)
            I.check(ink > 1500 and fill_px(s) < fa // 4,
                    "mermaid stale: the previous diagram stays up, dimmed, while editing",
                    "%d ink px, %d full-colour fill px" % (ink, fill_px(s)))
        time.sleep(3.0)
        n = P.Shot(env, tmp, win, "mm_md_new")
        if n.ok:
            I.check(fill_px(n) > fa, "mermaid stale: the new diagram replaces it",
                    "%d -> %d fill px" % (fa, fill_px(n)))
    finally:
        U.stop(p)
    time.sleep(0.5)
    alive = [k for k in kids if S.running(k)]
    I.check(not alive, "mermaid markdown: the helpers exit with the editor", "%s" % alive)


def case_slide(exe, env, tmp):
    d = S.proj(tmp, "mmslide")
    body = (b"---\nmarp: true\n---\n\n# A diagram on a slide\n\n```mermaid\n" + FLOW +
            b"```\n")
    p, log = I.launch_in(exe, env, d, "deck.md", body)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: slide (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "shift+F5", settle=3.5)
        s = P.Shot(env, tmp, win, "mm_slide")
        if not s.ok:
            print("skip: slide (no screenshot)")
            return
        # Marp's default theme is light: mermaid's default theme with the
        # slide's colours, the node fill its white mixed 12 % toward its
        # accent #0969da (core/img.ccs img_mm_opts).
        fill = tuple((b * 88 + a * 12 + 50) // 100 for b, a in zip((255, 255, 255), (9, 105, 218)))
        lav = I.count(s, fill, tol=6)
        I.check(lav > 1500, "mermaid slide: the diagram in the slide's (light) theme",
                "%d fill px %s" % (lav, fill))
    finally:
        U.stop(p)


def main(argv):
    exe = os.path.abspath(argv[1] if len(argv) > 1 else "bin/cctext-ui")
    if not os.path.exists(exe):
        print("skip: %s not built" % exe)
        return 0
    if not os.path.exists(os.path.join(os.path.dirname(exe), "cctext-render")):
        print("skip: no cctext-render beside %s" % exe)
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
        with tempfile.TemporaryDirectory(prefix="cctext_mm_") as tmp:
            for case in (case_markdown, case_slide):
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(I.FAILS))
    return len(I.FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

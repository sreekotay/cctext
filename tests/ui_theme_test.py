#!/usr/bin/env python3
"""cctext-ui themes under Xvfb (README "Settings", core/theme.cch).

The light pass of the key screenshots, and the live switch:

- Markdown Rich in the light theme: a light pane with dark text, a table
  header band in the light palette, a Mermaid diagram in Mermaid's light
  theme (its node fill from the light palette), display math in the dark
  text colour, and an SVG with no backing plate (the dark theme keeps
  the plate: the same file in dark has it);
- slides in the light theme: the letterbox is the light palette's, the
  deck keeps its own (Marp default: white) colours;
- the live switch: theme auto follows GTK's prefer-dark setting.
  RTX_UI_TEST_APPEARANCE=1 makes SIGUSR1 flip that setting inside the
  editor, so the flip takes the same notify:: path a desktop change does.
  The pane turns dark, the diagram re-renders in the dark theme, and the
  editor then idles with no wakeups (a signal, no polling); a second flip
  comes back to light;
- Toggle Light/Dark from the command palette flips the theme and pins it:
  a later OS change leaves it alone.

Screenshots: testdata/generated/ui_theme_light_markdown.png,
ui_theme_light_slide.png.

    python3 tests/ui_theme_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool, ImageMagick and bin/cctext-render
beside cctext-ui; skips otherwise. Exit status is the number of failures.
"""
import os
import shutil
import signal
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

# core/theme.cch (tests/theme_smoke.ccs holds the palette's contrast).
LIGHT_PANE = (253, 253, 254)
DARK_PANE = (28, 30, 36)
LIGHT_TBL_HDR = (232, 236, 244)
LIGHT_PRESENT = (214, 216, 222)
DARK_PRESENT = (12, 12, 14)
PLATE = S.PLATE


def mix(a, b, pct):
    """core/img.ccs img_mm_mix."""
    return tuple((x * (100 - pct) + y * pct + 50) // 100 for x, y in zip(a, b))


# Mermaid node fills: light = the light pane mixed 12 % toward the light
# accent; dark = the dark diagram bg mixed 22 % toward the dark accent.
LIGHT_FILL = mix((253, 253, 254), (0, 110, 200), 12)
DARK_FILL = mix((24, 24, 28), (110, 150, 230), 22)

FLOW = (b"flowchart LR\n  A[Start] --> B{Is it ok?}\n  B -->|yes| C[Ship]\n"
        b"  B -->|no| D[Fix]\n  D --> B\n")

DOC = (b"# Light theme\n\nSome *emphasis*, **bold**, `code` and ==marked== text.\n\n"
       b"| name | value |\n|------|-------|\n| alpha | 1 |\n| beta | 2 |\n\n"
       b"```mermaid\n" + FLOW + b"```\n\n![lines](lines.svg)\n\nend\n")

MATH = b"# M\n\nText.\n\n$$\n\\int_0^1 x^2\\,dx = \\frac{1}{3} \\qquad \\sum_{k=1}^{n} k\n$$\n\nend\n"


def launch(exe, env, d, name, body, theme, extra=None):
    e = {"RTX_THEME": theme} if theme else {}
    if extra:
        e.update(extra)
    return I.launch_in(exe, env, d, name, body, extra=e)


def env_auto(env):
    e = dict(env)
    e.pop("RTX_THEME", None)
    return e


def dark_ink(shot, x0, x1, y0, y1):
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            c = shot.at(x, y)
            if max(c) < 110:
                n += 1
    return n


def log_themes(log):
    try:
        with open(log) as f:
            return [l.split()[0] for l in f if l.startswith("theme=")]
    except OSError:
        return []


def wait_theme(log, want, n, secs=4.0):
    end = time.time() + secs
    while time.time() < end:
        t = log_themes(log)
        if len(t) >= n and t[-1] == want:
            return True
        time.sleep(0.1)
    return False


def case_markdown(exe, env, tmp):
    d = S.proj(tmp, "light_md")
    p, log = launch(exe, env, d, "doc.md", DOC, "light")
    try:
        win = I.window(env, p)
        if not win:
            print("skip: markdown (no window)")
            return
        time.sleep(4.0)
        a = P.Shot(env, tmp, win, "theme_md")
        if not a.ok:
            print("skip: markdown (no screenshot)")
            return
        pane, tot = a.count(lambda c: P.near(c, LIGHT_PANE, 3))
        I.check(pane > tot // 3, "light markdown: a light pane", "%d of %d px" % (pane, tot))
        I.check(log_themes(log)[:1] == ["theme=light"], "light markdown: RTX_THEME=light",
                "%s" % log_themes(log))
        ink = dark_ink(a, 60, a.w // 2, 30, 110)
        I.check(ink > 60, "light markdown: dark text on it", "%d dark px" % ink)
        hdr = I.count(a, LIGHT_TBL_HDR, tol=3)
        I.check(hdr > 150, "light markdown: the table header band", "%d px" % hdr)
        fill = I.count(a, LIGHT_FILL, tol=6)
        I.check(fill > 1500 and I.count(a, DARK_FILL, tol=6) < 50,
                "light markdown: the diagram in Mermaid's light theme (light palette fill)",
                "%d light fill px, %d dark" % (fill, I.count(a, DARK_FILL, tol=6)))
        # Below the diagram (Mermaid's own edge labels are near the plate's
        # colour): the transparent SVG's box.
        y0 = int(a.h * 0.66)
        plate, _ = a.count(lambda c: P.near(c, PLATE, 4), y0=y0)
        strokes, _ = a.count(lambda c: max(c) < 40, y0=y0)
        I.check(plate < 100 and strokes > 200, "light markdown: no backing plate under a transparent SVG",
                "%d plate px, %d stroke px" % (plate, strokes))
        S.save_crop(env, win, tmp, os.path.join(ROOT, "testdata", "generated",
                                                "ui_theme_light_markdown.png"))
        print("      screenshot: testdata/generated/ui_theme_light_markdown.png")
    finally:
        U.stop(p)
    # The same file in dark: the plate is there (svg_backing).
    p, log = launch(exe, env, d, "doc2.md", DOC, "dark")
    try:
        win = I.window(env, p)
        if not win:
            return
        time.sleep(4.0)
        b = P.Shot(env, tmp, win, "theme_md_dark")
        if b.ok:
            pane, tot = b.count(lambda c: P.near(c, DARK_PANE, 3))
            I.check(pane > tot // 3 and I.count(b, PLATE, tol=4) > 2000 and
                    I.count(b, DARK_FILL, tol=6) > 1500,
                    "dark markdown: dark pane, the SVG plate, Mermaid's dark theme",
                    "%d pane px, %d plate, %d fill" % (pane, I.count(b, PLATE, tol=4),
                                                      I.count(b, DARK_FILL, tol=6)))
    finally:
        U.stop(p)


def case_math(exe, env, tmp):
    """Display math takes the theme's text colour: dark ink on the light
    pane, light ink on the dark one."""
    d = S.proj(tmp, "light_math")
    res = {}
    for theme in ("light", "dark"):
        p, log = launch(exe, env, d, "m_%s.md" % theme, MATH, theme)
        try:
            win = I.window(env, p)
            if not win:
                print("skip: math (no window)")
                return
            time.sleep(3.0)
            s = P.Shot(env, tmp, win, "theme_math_" + theme)
            if not s.ok:
                return
            x0, x1 = s.w // 4, 3 * s.w // 4
            dark = dark_ink(s, x0, x1, 60, 260)
            light = 0
            for y in range(60, 260, 2):
                for x in range(x0, x1, 2):
                    if min(s.at(x, y)) > 170:
                        light += 1
            res[theme] = (dark, light)
        finally:
            U.stop(p)
    lt, dk = res.get("light"), res.get("dark")
    # (dark px, light px) in the centre band, where only the formula is:
    # the light pane counts as light, the dark one as dark.
    I.check(bool(lt) and lt[0] > 60, "light math: the formula in dark ink", "%s" % (lt,))
    I.check(bool(dk) and dk[1] > 60 and dk[1] < 3000, "dark math: the formula in light ink",
            "%s" % (dk,))


def case_slide(exe, env, tmp):
    d = S.proj(tmp, "light_slide")
    body = (b"---\nmarp: true\n---\n\n# A slide\n\nWords on the slide.\n\n```mermaid\n" + FLOW +
            b"```\n")
    p, log = launch(exe, env, d, "deck.md", body, "light")
    try:
        win = I.window(env, p)
        if not win:
            print("skip: slide (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "shift+F5", settle=3.5)
        s = P.Shot(env, tmp, win, "theme_slide")
        if not s.ok:
            print("skip: slide (no screenshot)")
            return
        bar, _ = s.count(lambda c: P.near(c, LIGHT_PRESENT, 3))
        darkbar, _ = s.count(lambda c: P.near(c, DARK_PRESENT, 3))
        white, _ = s.count(P.white)
        I.check(bar > 500 and darkbar < 50,
                "light slide: the letterbox is the light palette's", "%d light, %d dark" % (bar, darkbar))
        I.check(white > 20000, "light slide: the deck keeps its own (white) theme", "%d white" % white)
        slide_fill = mix((255, 255, 255), (0x09, 0x69, 0xda), 12)
        I.check(I.count(s, slide_fill, tol=6) > 1500,
                "light slide: the diagram in the slide's light colours", "%d px" % I.count(s, slide_fill, tol=6))
        S.save_crop(env, win, tmp, os.path.join(ROOT, "testdata", "generated",
                                                "ui_theme_light_slide.png"))
        print("      screenshot: testdata/generated/ui_theme_light_slide.png")
    finally:
        U.stop(p)


def case_live_switch(exe, env, tmp):
    """Theme auto follows GTK's prefer-dark, live."""
    d = S.proj(tmp, "live")
    body = b"# Live\n\nText.\n\n```mermaid\n" + FLOW + b"```\n\nend\n"
    e = env_auto(env)
    p, log = I.launch_in(exe, e, d, "live.md", body, extra={"RTX_UI_TEST_APPEARANCE": "1"})
    try:
        win = I.window(e, p)
        if not win:
            print("skip: live switch (no window)")
            return
        time.sleep(3.0)
        a = P.Shot(e, tmp, win, "live_a")
        # Xvfb's GTK: Adwaita, prefer-dark off: auto is light.
        I.check(log_themes(log)[:1] == ["theme=light"], "live: auto starts light (GTK light)",
                "%s" % log_themes(log))
        if a.ok:
            I.check(I.count(a, LIGHT_FILL, tol=6) > 1500, "live: the diagram in the light theme")
        os.kill(p.pid, signal.SIGUSR1)
        ok = wait_theme(log, "theme=dark", 2)
        I.check(ok, "live: prefer-dark on: the editor turns dark", "%s" % log_themes(log))
        time.sleep(3.0)
        b = P.Shot(e, tmp, win, "live_b")
        if b.ok:
            pane, tot = b.count(lambda c: P.near(c, DARK_PANE, 3))
            I.check(pane > tot // 3, "live: the pane repaints dark", "%d of %d" % (pane, tot))
            I.check(I.count(b, DARK_FILL, tol=6) > 1500 and I.count(b, LIGHT_FILL, tol=6) < 50,
                    "live: the diagram re-renders in the dark theme",
                    "%d dark fill, %d light" % (I.count(b, DARK_FILL, tol=6),
                                                I.count(b, LIGHT_FILL, tol=6)))
        # No polling for the appearance: idle after the switch settles.
        I.check_idle("live switch", p.pid)
        os.kill(p.pid, signal.SIGUSR1)
        ok = wait_theme(log, "theme=light", 3)
        I.check(ok, "live: prefer-dark off: back to light", "%s" % log_themes(log))
        time.sleep(1.5)
        c = P.Shot(e, tmp, win, "live_c")
        if c.ok:
            I.check(I.count(c, LIGHT_FILL, tol=6) > 1500,
                    "live: the light diagram is back (from the cache)")
    finally:
        U.stop(p)


def case_toggle(exe, env, tmp):
    """Toggle Light/Dark from the palette flips and pins the theme."""
    d = S.proj(tmp, "toggle")
    e = env_auto(env)
    p, log = I.launch_in(exe, e, d, "t.md", b"# Toggle\n\nwords\n",
                         extra={"RTX_UI_TEST_APPEARANCE": "1"})
    try:
        win = I.window(e, p)
        if not win:
            print("skip: toggle (no window)")
            return
        time.sleep(1.5)
        I.key(e, win, "F1", settle=0.5)
        U.xdo(e, "type", "--window", win, "--delay", "20", "Toggle Light")
        time.sleep(0.4)
        I.key(e, win, "Return", settle=1.0)
        I.check(wait_theme(log, "theme=dark", 2), "toggle: the palette command flips light to dark",
                "%s" % log_themes(log))
        with open(log) as f:
            modes = [l.strip() for l in f if l.startswith("theme=")]
        I.check(modes and modes[-1].endswith("mode=dark"), "toggle: and pins the mode",
                "%s" % modes)
        s = P.Shot(e, tmp, win, "toggle_b")
        if s.ok:
            pane, tot = s.count(lambda c: P.near(c, DARK_PANE, 3))
            I.check(pane > tot // 3, "toggle: the pane repaints dark", "%d of %d" % (pane, tot))
        os.kill(p.pid, signal.SIGUSR1)
        time.sleep(1.0)
        I.check(log_themes(log)[-1] == "theme=dark",
                "toggle: a later OS change leaves a pinned theme alone", "%s" % log_themes(log))
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
    names = argv[2:]
    cases = [case_markdown, case_math, case_slide, case_live_switch, case_toggle]
    try:
        with tempfile.TemporaryDirectory(prefix="cctext_theme_") as tmp:
            for case in cases:
                if names and case.__name__[5:] not in names:
                    continue
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(I.FAILS))
    return len(I.FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

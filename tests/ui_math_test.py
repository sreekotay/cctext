#!/usr/bin/env python3
"""cctext-ui math under Xvfb (docs/images.md, "Math").

Coarse properties of screenshots, as tests/ui_mermaid_test.py:

- a Markdown file with a `$$` block and inline math: the display formula
  is a centred band of ink where the block's lines were, and it stays put
  while the window idles; with "math": false the same file is source text
  (no centred band);
- inline math: a fraction in a line of text is a picture set on the
  line's baseline (it reaches above and below the text beside it, centred
  on it) and the line grows to hold it;
- one helper process (the math slot), gone when the editor exits; idle
  once the formulas settle;
- the caret into the block shows its source with the formula under it;
  a typed edit keeps the old formula up, dimmed, until the new one lands;
- a Marp slide's `$$` block in the slide's (dark on light) text colour.

The Markdown screenshot is kept as testdata/generated/ui_math_markdown.png.

    python3 tests/ui_math_test.py [path/to/cctext-ui]

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

DOC = (b"# Math\n\nBefore the block.\n\n$$\n"
       b"\\sum_{k=1}^{n} k^2 = \\frac{n(n+1)(2n+1)}{6}\n$$\n\nAfter the block.\n\n"
       b"Inline $\\frac{a+b}{c}$ tail\n\nLast line.\n")


def lum(c):
    return (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000


def bands(shot, x0, x1, thr=150, y0=30):
    """Horizontal bands of rows holding bright (text / formula) pixels in
    [x0, x1): (top, bottom, min_x, max_x, max_lum)."""
    out = []
    cur = None
    for y in range(y0, shot.h - 30):
        xs = [x for x in range(x0, x1) if lum(shot.at(x, y)) > thr]
        if xs:
            peak = max(lum(shot.at(x, y)) for x in xs)
            if cur is None:
                cur = [y, y, min(xs), max(xs), peak]
            else:
                cur[1] = y
                cur[2] = min(cur[2], min(xs))
                cur[3] = max(cur[3], max(xs))
                cur[4] = max(cur[4], peak)
        elif cur is not None:
            out.append(tuple(cur))
            cur = None
    if cur is not None:
        out.append(tuple(cur))
    return out


def centred(shot):
    """The display formula's band: ink that starts well right of the text
    margin and is at least two text lines tall."""
    for b in bands(shot, 60, shot.w - 40, thr=120):
        top, bot, mn, mx = b[0], b[1], b[2], b[3]
        if mn > 180 and bot - top >= 30:
            return b
    return None


def ink_rows(shot, x0, x1, y0, y1, thr=150):
    ys = [y for y in range(y0, y1) if any(lum(shot.at(x, y)) > thr for x in range(x0, x1))]
    return (min(ys), max(ys)) if ys else None


def case_markdown(exe, env, tmp):
    d = S.proj(tmp, "math")
    p, log = I.launch_in(exe, env, d, "doc.md", DOC)
    kids = []
    try:
        win = I.window(env, p)
        if not win:
            print("skip: markdown (no window)")
            return
        time.sleep(3.0)
        a = P.Shot(env, tmp, win, "math_a")
        time.sleep(1.0)
        b = P.Shot(env, tmp, win, "math_b")
        if not (a.ok and b.ok):
            print("skip: markdown (no screenshot)")
            return
        ca, cb = centred(a), centred(b)
        mid = a.w // 2
        I.check(ca is not None and abs((ca[2] + ca[3]) // 2 - mid) < 60 and ca[1] - ca[0] >= 40,
                "math markdown: the display formula is a centred band", "%s" % (ca,))
        I.check(ca is not None and ca == cb, "math markdown: the formula stays put", "%s -> %s" % (ca, cb))
        S.save_crop(env, win, tmp, os.path.join(ROOT, "testdata", "generated",
                                                "ui_math_markdown.png"))
        print("      screenshot: testdata/generated/ui_math_markdown.png")
        # Inline: "Inline" (text) then the fraction; the fraction's ink
        # reaches above and below the word's and is centred on it (the
        # fraction bar sits on the math axis, near the text's middle).
        # Bands below the display formula, near ones merged (a fraction's
        # denominator is apart from its bar by a blank row or two): the
        # second left-aligned one is the line with the inline fraction.
        merged = []
        for bd in bands(a, 55, a.w - 40):
            if ca is not None and bd[0] <= ca[1]:
                continue
            if merged and bd[0] - merged[-1][1] <= 5:
                m = merged[-1]
                merged[-1] = (m[0], bd[1], min(m[2], bd[2]), max(m[3], bd[3]), max(m[4], bd[4]))
            else:
                merged.append(bd)
        lines = [bd for bd in merged if bd[2] < 80]
        inline = lines[1] if len(lines) > 1 else None
        if inline and inline[1] - inline[0] < 24:
            inline = None
        if inline:
            word = ink_rows(a, 55, 100, inline[0], inline[1] + 1)
            frac = ink_rows(a, 120, 180, inline[0], inline[1] + 1)
            ok = word and frac and frac[0] < word[0] - 3 and frac[1] > word[1] + 1
            if ok:
                wc, fc = (word[0] + word[1]) / 2.0, (frac[0] + frac[1]) / 2.0
                ok = abs(wc - fc) <= 5
            I.check(bool(ok), "math inline: the fraction sits on the text's baseline, centred on it",
                    "word rows %s, fraction rows %s" % (word, frac))
        else:
            I.check(False, "math inline: a line with an inline fraction taller than text",
                    "%s" % (bands(a, 60, a.w - 40),))
        kids = S.helpers_of(p.pid)
        I.check(len(kids) == 1, "math markdown: one helper (the math slot)", "%s" % kids)
        I.check_idle("math markdown", p.pid)
        # Caret into the block: the source, the formula under it.
        I.key(env, win, "ctrl+Home", settle=0.3)
        for _ in range(5):
            I.key(env, win, "Down", settle=0.15)
        time.sleep(1.0)
        c = P.Shot(env, tmp, win, "math_c")
        cc = centred(c) if c.ok else None
        I.check(ca is not None and cc is not None and cc[0] > ca[0] + 30,
                "math markdown: the caret shows the block's source, the formula under it",
                "%s -> %s" % (ca, cc))
        # Type into the formula: the old one stays, dimmed, then the new one.
        I.key(env, win, "End", settle=0.2)
        U.xdo(env, "type", "--window", win, "--delay", "15", " + 1")
        s = P.Shot(env, tmp, win, "math_stale")
        if s.ok and cc is not None:
            dim = ink_rows(s, 180, s.w - 180, cc[0] - 4, cc[1] + 30, thr=60)
            bright = ink_rows(s, 180, s.w - 180, cc[0] - 4, cc[1] + 30, thr=170)
            I.check(dim is not None and bright is None,
                    "math stale: the previous formula stays up, dimmed, while editing",
                    "dim rows %s, bright rows %s" % (dim, bright))
        time.sleep(2.5)
        n = P.Shot(env, tmp, win, "math_new")
        if n.ok:
            cn = centred(n)
            I.check(cn is not None and cn[4] > 170 and cc is not None and cn[3] - cn[2] > cc[3] - cc[2],
                    "math stale: the new formula (wider: + 1) replaces it", "%s -> %s" % (cc, cn))
    finally:
        U.stop(p)
    time.sleep(0.5)
    alive = [k for k in kids if S.running(k)]
    I.check(not alive, "math markdown: the helper exits with the editor", "%s" % alive)


def case_off(exe, env, tmp):
    """"math": false: the same file is text (no centred band)."""
    d = S.proj(tmp, "mathoff")
    sp = os.path.join(d, "settings.json")
    with open(sp, "w") as f:
        f.write('{"math": false}')
    p, log = I.launch_in(exe, env, d, "doc.md", DOC, extra={"RTX_SETTINGS": sp})
    try:
        win = I.window(env, p)
        if not win:
            print("skip: off (no window)")
            return
        time.sleep(2.0)
        a = P.Shot(env, tmp, win, "math_off")
        if a.ok:
            I.check(centred(a) is None and not S.helpers_of(p.pid),
                    "math off: source text, no picture, no helper")
    finally:
        U.stop(p)


def case_slide(exe, env, tmp):
    d = S.proj(tmp, "mathslide")
    body = (b"---\nmarp: true\n---\n\n# A formula on a slide\n\n$$\n"
            b"E = mc^2 \\qquad \\int_0^1 x\\,dx = \\frac{1}{2}\n$$\n")
    p, log = I.launch_in(exe, env, d, "deck.md", body)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: slide (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "shift+F5", settle=3.0)
        s = P.Shot(env, tmp, win, "math_slide")
        if not s.ok:
            print("skip: slide (no screenshot)")
            return
        # Marp's default theme: dark text on a light slide. The formula is
        # drawn in that text colour, below the heading, centred.
        dark = 0
        xs = []
        for y in range(int(s.h * 0.28), int(s.h * 0.8), 2):
            for x in range(0, s.w, 2):
                c = s.at(x, y)
                if lum(c) < 90:
                    dark += 1
                    xs.append(x)
        mid = (min(xs) + max(xs)) // 2 if xs else 0
        I.check(dark > 60 and abs(mid - s.w // 2) < s.w // 8,
                "math slide: the formula in the slide's text colour, centred",
                "%d dark px, centre %d of %d" % (dark, mid, s.w))
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
        with tempfile.TemporaryDirectory(prefix="cctext_math_") as tmp:
            for case in (case_markdown, case_off, case_slide):
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(I.FAILS))
    return len(I.FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

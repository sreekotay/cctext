#!/usr/bin/env python3
"""cctext-ui SVG images under Xvfb (docs/images.md, "Renderer").

Coarse properties of screenshots, as tests/ui_img_test.py:

- a Markdown file with `![...](pipe.svg)`: the picture is on screen (the
  fixture's red / green / blue blocks, drawn by cctext-render), it stays
  put while the window idles and when its pixels arrive late (the
  self-test helper), and a transparent SVG gets its light
  backing plate on the dark pane;
- an .svg opened directly is text; Ctrl-D swaps in its picture and back;
- a Marp slide's `![bg left:40%](pipe.svg)` fills its side;
- the browse preview of an .svg is its picture;
- the helper process: one, started by the first SVG, gone once the editor
  exits.

The Markdown screenshot is kept as testdata/generated/ui_svg_markdown.png.

    python3 tests/ui_svg_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool, ImageMagick and bin/cctext-render
beside cctext-ui; skips otherwise. Exit status is the number of failures.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402
import ui_present_test as P  # noqa: E402
import ui_img_test as I  # noqa: E402

RED, GREEN, BLUE = (230, 20, 20), (20, 180, 40), (30, 60, 220)
PLATE = (244, 244, 240)

# Big flat blocks (easy to count), a label, and a transparent corner.
PIPE = b"""<svg xmlns="http://www.w3.org/2000/svg" width="420" height="240" viewBox="0 0 420 240">
  <rect x="0" y="0" width="140" height="160" fill="#e61414"/>
  <rect x="140" y="0" width="140" height="160" fill="#14b428"/>
  <rect x="280" y="0" width="140" height="160" fill="#1e3cdc"/>
  <text x="210" y="210" font-family="Helvetica" font-size="28" text-anchor="middle">cctext-render</text>
</svg>
"""
# Black strokes on transparency: invisible on a dark pane without the plate.
LINES = b"""<svg xmlns="http://www.w3.org/2000/svg" width="300" height="160">
  <path d="M10 150 L150 10 L290 150" fill="none" stroke="#000" stroke-width="6"/>
</svg>
"""


def proj(tmp, name):
    d = os.path.join(tmp, name)
    os.makedirs(os.path.join(d, ".git"), exist_ok=True)
    with open(os.path.join(d, "pipe.svg"), "wb") as f:
        f.write(PIPE)
    with open(os.path.join(d, "lines.svg"), "wb") as f:
        f.write(LINES)
    return d


def helpers_of(pid):
    """cctext-render processes whose parent is pid."""
    out = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % e) as f:
                st = f.read()
            with open("/proc/%s/cmdline" % e, "rb") as f:
                cmd = f.read()
        except OSError:
            continue
        ppid = int(st.rsplit(")", 1)[1].split()[1])
        if ppid == pid and b"cctext-render" in cmd:
            out.append(int(e))
    return out


def running(pid):
    """Alive and not a zombie (an orphan's parent in a container may never
    reap it)."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def save_crop(env, win, tmp, out):
    geo = subprocess.run(["xdotool", "getwindowgeometry", "--shell", win], env=env,
                         capture_output=True, text=True).stdout
    vals = dict(l.split("=", 1) for l in geo.split() if "=" in l)
    png = os.path.join(tmp, "root_keep.png")
    subprocess.run(["import", "-window", "root", png], env=env, capture_output=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(["convert", png, "-crop", "%sx%s+%s+%s" % (vals.get("WIDTH"), vals.get("HEIGHT"),
                                                              vals.get("X"), vals.get("Y")),
                    "+repage", out], capture_output=True)


def case_markdown(exe, env, tmp):
    d = proj(tmp, "md")
    body = (b"# SVG in Markdown\n\nA diagram drawn by the sandboxed renderer:\n\n"
            b"![pipeline](pipe.svg)\n\nText after the picture.\n\n![lines](lines.svg)\n\nend\n")
    p, log = I.launch_in(exe, env, d, "doc.md", body)
    kids = []
    try:
        win = I.window(env, p)
        if not win:
            print("skip: markdown (no window)")
            return
        time.sleep(2.0)
        a = P.Shot(env, tmp, win, "svg_md_a")
        time.sleep(0.8)
        b = P.Shot(env, tmp, win, "svg_md_b")
        if not (a.ok and b.ok):
            print("skip: markdown (no screenshot)")
            return
        r, g, bl = I.count(a, RED), I.count(a, GREEN), I.count(a, BLUE)
        I.check(r > 2000 and g > 2000 and bl > 2000, "svg markdown: the picture is on screen",
                "red %d green %d blue %d px" % (r, g, bl))
        t0, t1 = I.top_of(a, RED), I.top_of(b, RED)
        I.check(t0 is not None and t0 == t1, "svg markdown: the picture stays put",
                "%s -> %s" % (t0, t1))
        plate = I.count(a, PLATE, tol=4)
        I.check(plate > 2000, "svg markdown: a light plate under a transparent SVG",
                "%d plate px" % plate)
        kids = helpers_of(p.pid)
        I.check(len(kids) == 1, "svg markdown: one helper process", "%s" % kids)
        save_crop(env, win, tmp, os.path.join(ROOT, "testdata", "generated", "ui_svg_markdown.png"))
        print("      screenshot: testdata/generated/ui_svg_markdown.png")
    finally:
        U.stop(p)
    time.sleep(0.5)
    alive = [k for k in kids if running(k)]
    I.check(not alive, "svg markdown: the helper exits with the editor", "%s" % alive)


PURPLE = (142, 36, 170)


def case_stable(exe, env, tmp):
    """The SVG's size first, its pixels 1.5 s later (the self-test helper's
    `slowpx`): the picture below it does not move when they arrive."""
    selftest = os.path.join(os.path.dirname(exe), "cctext-render-selftest")
    if not os.path.exists(selftest):
        print("skip: stable (no cctext-render-selftest)")
        return
    d = proj(tmp, "stable")
    with open(os.path.join(d, "slow.svg"), "wb") as f:
        f.write(b"<!--cr-selftest:slowpx-->" + PIPE.replace(b"#e61414", b"#8e24aa")
                .replace(b"#14b428", b"#8e24aa").replace(b"#1e3cdc", b"#8e24aa"))
    shutil.copy(os.path.join(I.IMG, "big.png"), d)
    body = b"# Stable\n\n![slow](slow.svg)\n\n![big](big.png)\n\nend\n"
    p, log = I.launch_in(exe, env, d, "doc.md", body, {"RTX_RENDER_BIN": selftest})
    try:
        win = I.window(env, p)
        if not win:
            print("skip: stable (no window)")
            return
        time.sleep(0.5)
        a = P.Shot(env, tmp, win, "svg_stable_a")
        time.sleep(2.5)
        b = P.Shot(env, tmp, win, "svg_stable_b")
        if not (a.ok and b.ok):
            print("skip: stable (no screenshot)")
            return
        pa, pb = I.count(a, PURPLE, tol=10), I.count(b, PURPLE, tol=10)
        if pa > 100:
            print("skip: stable (the pixels beat the first screenshot: %d px)" % pa)
            return
        ta, tb = I.top_of(a, I.RED), I.top_of(b, I.RED)
        I.check(pb > 2000, "svg stable: the SVG's pixels arrive late", "%d -> %d purple px" % (pa, pb))
        I.check(ta is not None and ta == tb, "svg stable: the picture below does not move",
                "big.png top %s -> %s" % (ta, tb))
    finally:
        U.stop(p)


def case_viewer(exe, env, tmp):
    d = proj(tmp, "view")
    p, log = I.launch_in(exe, env, d, "pipe.svg", None)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: viewer (no window)")
            return
        time.sleep(1.2)
        a = P.Shot(env, tmp, win, "svg_view_a")
        I.key(env, win, "ctrl+d", settle=1.5)
        b = P.Shot(env, tmp, win, "svg_view_b")
        I.key(env, win, "ctrl+d", settle=1.0)
        c = P.Shot(env, tmp, win, "svg_view_c")
        if not (a.ok and b.ok and c.ok):
            print("skip: viewer (no screenshot)")
            return
        I.check(I.count(a, RED) < 50, "svg viewer: an .svg opens as text", "%d red px" % I.count(a, RED))
        I.check(I.count(b, RED) > 3000 and I.count(b, BLUE) > 3000,
                "svg viewer: Ctrl-D shows its picture", "%d red px" % I.count(b, RED))
        I.check(I.count(c, RED) < 50, "svg viewer: Ctrl-D again, its text", "%d red px" % I.count(c, RED))
    finally:
        U.stop(p)


def case_slide(exe, env, tmp):
    d = proj(tmp, "deck")
    body = b"---\nmarp: true\n---\n\n# Title\n\n![bg left:40%](pipe.svg)\n\nText beside.\n"
    p, log = I.launch_in(exe, env, d, "deck.md", body)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: slide (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "shift+F5", settle=2.0)
        s = P.Shot(env, tmp, win, "svg_slide")
        if not s.ok:
            print("skip: slide (no screenshot)")
            return
        n = I.count(s, RED) + I.count(s, GREEN) + I.count(s, BLUE)
        right = sum(1 for y in range(0, s.h, 6) for x in range(int(s.w * 0.5), s.w, 6)
                    if P.near(s.at(x, y), RED, 16) or P.near(s.at(x, y), BLUE, 16))
        I.check(n > 5000, "svg slide: the bg picture is drawn", "%d block px" % n)
        I.check(right == 0, "svg slide: the text side has no picture", "%d" % right)
    finally:
        U.stop(p)


def case_browse(exe, env, tmp):
    d = proj(tmp, "browse")
    os.remove(os.path.join(d, "lines.svg"))
    e = dict(env)
    e.update({"RTX_SAFE_HOME": os.path.join(tmp, "safe_browse"), "RTX_BLINK_IDLE_MS": "300"})
    p = subprocess.Popen([exe, d], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: browse (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "Down", settle=1.8)
        s = P.Shot(env, tmp, win, "svg_browse")
        if not s.ok:
            print("skip: browse (no screenshot)")
            return
        I.check(I.count(s, RED) > 800 and I.count(s, BLUE) > 800,
                "svg browse: an .svg's preview is its picture", "%d red px" % I.count(s, RED))
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
        with tempfile.TemporaryDirectory(prefix="cctext_svg_") as tmp:
            for case in (case_markdown, case_stable, case_viewer, case_slide, case_browse):
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(I.FAILS))
    return len(I.FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

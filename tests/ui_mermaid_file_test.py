#!/usr/bin/env python3
"""cctext-ui Mermaid files under Xvfb (docs/images.md, "Mermaid files").

Coarse properties of screenshots, as tests/ui_mermaid_test.py:

- a `.mmd` whose first word is a diagram keyword opens as text; Ctrl-D
  shows its diagram (the dark theme's node fill, drawn by cctext-render's
  Mermaid slot) and Ctrl-D again its text;
- an edit made in the text shows, on the next Ctrl-D, the previous
  diagram dimmed until the new one lands (the stale display), then the
  new one;
- a `.mmd` that is MultiMarkdown opens as Markdown: Ctrl-D is the Rich
  lens, no diagram;
- the browse preview of a `.mermaid` file is its diagram.

    python3 tests/ui_mermaid_file_test.py [path/to/cctext-ui]

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
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402
import ui_present_test as P  # noqa: E402
import ui_img_test as I  # noqa: E402
import ui_svg_test as S  # noqa: E402
import ui_mermaid_test as M  # noqa: E402

FLOW = (b"---\ntitle: Review\n---\n%% a Mermaid Live diagram\n" + M.FLOW)


def case_viewer(exe, env, tmp):
    d = S.proj(tmp, "mmdview")
    p, log = I.launch_in(exe, env, d, "flow.mmd", FLOW)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: viewer (no window)")
            return
        time.sleep(1.2)
        a = P.Shot(env, tmp, win, "mmd_view_a")
        I.key(env, win, "ctrl+d", settle=3.5)
        b = P.Shot(env, tmp, win, "mmd_view_b")
        I.key(env, win, "ctrl+d", settle=1.0)
        c = P.Shot(env, tmp, win, "mmd_view_c")
        if not (a.ok and b.ok and c.ok):
            print("skip: viewer (no screenshot)")
            return
        fb = M.fill_px(b)
        I.check(M.fill_px(a) < 200, "mermaid file: a .mmd opens as text",
                "%d fill px" % M.fill_px(a))
        I.check(fb > 1500, "mermaid file: Ctrl-D shows its diagram", "%d fill px" % fb)
        I.check(M.fill_px(c) < 200, "mermaid file: Ctrl-D again, its text",
                "%d fill px" % M.fill_px(c))
        # Edit the text, then look again: the old diagram, dimmed, at once;
        # the new one (a node more) when it lands.
        for _ in range(9):
            I.key(env, win, "Down", settle=0.05)
        I.key(env, win, "End", "Return", settle=0.1)
        U.xdo(env, "type", "--window", win, "--delay", "20", "  C --> E[Extra step]")
        time.sleep(0.3)
        I.key(env, win, "ctrl+d", settle=0.05)
        s = P.Shot(env, tmp, win, "mmd_view_stale")
        if s.ok:
            bg = s.at(s.w - 40, 60)
            ink = sum(1 for y in range(40, s.h - 60, 3) for x in range(20, s.w - 20, 3)
                      if sum(abs(q - r) for q, r in zip(s.at(x, y), bg)) > 12)
            I.check(ink > 300 and M.fill_px(s) < fb // 4,
                    "mermaid file: after an edit the previous diagram stays up, dimmed",
                    "%d ink px, %d full-colour fill px" % (ink, M.fill_px(s)))
        time.sleep(3.5)
        n = P.Shot(env, tmp, win, "mmd_view_new")
        if n.ok:
            I.check(M.fill_px(n) > 1500, "mermaid file: the edited diagram replaces it",
                    "%d -> %d fill px" % (fb, M.fill_px(n)))
    finally:
        U.stop(p)


def case_multimarkdown(exe, env, tmp):
    d = S.proj(tmp, "mmdmd")
    body = b"Title: Notes\nAuthor: Me\n\n# Notes\n\ngraph paper and *emphasis*\n"
    p, log = I.launch_in(exe, env, d, "notes.mmd", body)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: multimarkdown (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "ctrl+d", settle=2.0)
        s = P.Shot(env, tmp, win, "mmd_md")
        if not s.ok:
            print("skip: multimarkdown (no screenshot)")
            return
        I.check(p.poll() is None and M.fill_px(s) < 200,
                "mermaid file: a MultiMarkdown .mmd is Markdown (Ctrl-D: no diagram)",
                "%d fill px" % M.fill_px(s))
    finally:
        U.stop(p)


def case_browse(exe, env, tmp):
    d = os.path.join(tmp, "mmdbrowse")
    os.makedirs(os.path.join(d, ".git"), exist_ok=True)
    with open(os.path.join(d, "flow.mermaid"), "wb") as f:
        f.write(M.FLOW)
    e = dict(env)
    e.update({"RTX_SAFE_HOME": os.path.join(tmp, "safe_mmd_browse"), "RTX_BLINK_IDLE_MS": "300"})
    p = subprocess.Popen([exe, d], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        win = I.window(env, p)
        if not win:
            print("skip: browse (no window)")
            return
        time.sleep(1.0)
        I.key(env, win, "Down", settle=3.5)
        s = P.Shot(env, tmp, win, "mmd_browse")
        if not s.ok:
            print("skip: browse (no screenshot)")
            return
        I.check(M.fill_px(s) > 600, "mermaid file: a .mermaid's browse preview is its diagram",
                "%d fill px" % M.fill_px(s))
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
        with tempfile.TemporaryDirectory(prefix="cctext_mmd_") as tmp:
            for case in (case_viewer, case_multimarkdown, case_browse):
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(I.FAILS))
    return len(I.FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

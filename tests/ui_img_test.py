#!/usr/bin/env python3
"""cctext-ui images under Xvfb (docs/images.md).

Coarse properties of screenshots, never exact pixels:

- a Markdown file: the picture of `![big](big.png)` is on screen (the
  fixture's red / green / blue quadrants), a missing image is a
  placeholder, and the picture does not move once decoded;
- the caret on the image's line shows its source above the picture (the
  picture moves down by about one line);
- a Marp deck (Shift-F5): `![bg left:40%](big.png)` fills the left of the
  slide with the picture, the text keeps the right;
- an image file opened directly is the viewer (the picture), Ctrl-D is
  its hex (no picture);
- the browse preview of a folder's image file is the picture;
- a remote image: a placeholder; a click asks (the dialog runs, answered
  "Load this image" through RTX_UI_SCRIPT) and the picture arrives from a
  local HTTP server that saw no request before the click;
- idle with pictures on screen: no paints and (almost) no CPU;
- `"image_animate": true`: an animated GIF paints its frames.

    python3 tests/ui_img_test.py [path/to/cctext-ui]

Needs Xvfb (or $DISPLAY), xdotool and ImageMagick; skips otherwise. Exit
status is the number of failed checks.
"""
import http.server
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_blink_test as U  # noqa: E402
import ui_present_test as P  # noqa: E402

IMG = os.path.join(HERE, "..", "testdata", "img")
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


def count(shot, rgb, tol=14):
    n, _ = shot.count(lambda c: P.near(c, rgb, tol), step=2)
    return n


def top_of(shot, rgb, tol=14):
    for y in range(0, shot.h, 1):
        hits = sum(1 for x in range(0, shot.w, 4) if P.near(shot.at(x, y), rgb, tol))
        if hits > 10:
            return y
    return None


def window(env, p):
    for _ in range(60):
        time.sleep(0.25)
        win = U.xdo(env, "search", "--onlyvisible", "--pid", str(p.pid))
        if win or p.poll() is not None:
            return win[0] if win else None
    return None


def proj(tmp, name):
    d = os.path.join(tmp, name)
    os.makedirs(os.path.join(d, ".git"), exist_ok=True)
    for f in ("big.png", "quad.png"):
        shutil.copy(os.path.join(IMG, f), d)
    return d


def launch_in(exe, env, d, name, body, extra=None, args=()):
    path = os.path.join(d, name)
    if body is not None:
        with open(path, "wb") as f:
            f.write(body)
    log = path + ".log"
    e = dict(env)
    e.update({"RTX_UI_LOG": log, "RTX_SAFE_HOME": os.path.join(d, "..", "safe_" + name),
              "RTX_BLINK_IDLE_MS": "300"})
    e.pop("RTX_UI_SCRIPT", None)
    if extra:
        e.update(extra)
    p = subprocess.Popen([exe] + list(args) + [path], env=e, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    return p, log


def key(env, win, *keys, settle=0.5):
    for k in keys:
        U.xdo(env, "key", "--window", win, k)
        time.sleep(0.08)
    time.sleep(settle)


def case_markdown(exe, env, tmp):
    d = proj(tmp, "md")
    body = (b"# Pictures\n\nSome text.\n\n![big one](big.png)\n\n![gone](missing.png)\n\n"
            b"The end.\n")
    p, log = launch_in(exe, env, d, "doc.md", body)
    try:
        win = window(env, p)
        if not win:
            print("skip: markdown (no window)")
            return
        time.sleep(1.2)
        a = P.Shot(env, tmp, win, "md_a")
        time.sleep(0.6)
        b = P.Shot(env, tmp, win, "md_b")
        if not (a.ok and b.ok):
            print("skip: markdown (no screenshot)")
            return
        r, g, bl = count(a, RED), count(a, GREEN), count(a, BLUE)
        check(r > 2000 and g > 2000 and bl > 2000, "markdown: the picture is on screen",
              "red %d green %d blue %d px" % (r, g, bl))
        t0, t1 = top_of(a, RED), top_of(b, RED)
        check(t0 is not None and t0 == t1, "markdown: the picture stays put", "%s -> %s" % (t0, t1))
        # The caret onto the image's line: its source shows above it.
        key(env, win, "Down", "Down", "Down", "Down", settle=0.9)
        c = P.Shot(env, tmp, win, "md_c")
        t2 = top_of(c, RED)
        check(t0 is not None and t2 is not None and 10 <= t2 - t0 <= 40,
              "markdown: caret on the line shows the source above the picture",
              "picture top %s -> %s" % (t0, t2))
        # Idle with the picture on screen: nothing paints, the CPU rests.
        key(env, win, "Up", "Up", "Up", "Up", settle=1.0)
        n0 = len(U.read_log(log))
        c0 = P.cpu_ticks(p.pid)
        time.sleep(1.6)
        paints = [l for l in U.read_log(log)[n0:] if l.endswith("paint full")]
        dc = P.cpu_ticks(p.pid) - c0
        check(len(paints) == 0 and dc <= 3, "markdown: idle with a picture paints nothing",
              "%d paints, %d CPU ticks in 1.6 s" % (len(paints), dc))
    finally:
        U.stop(p)


def case_anim(exe, env, tmp):
    """`image_animate`: an animated GIF paints its frames (and only while
    on screen); off (case_markdown) a GIF is its first frame, no paints."""
    d = proj(tmp, "anim")
    shutil.copy(os.path.join(IMG, "anim.gif"), d)
    settings = os.path.join(d, "settings.json")
    with open(settings, "w") as f:
        f.write('{ "image_animate": true }\n')
    p, log = launch_in(exe, env, d, "doc.md", b"# Anim\n\n![spin](anim.gif)\n\nend\n",
                       args=("--settings", settings))
    try:
        win = window(env, p)
        if not win:
            print("skip: anim (no window)")
            return
        time.sleep(1.5)
        n0 = len(U.read_log(log))
        time.sleep(1.3)
        paints = [l for l in U.read_log(log)[n0:] if l.endswith("paint full")]
        # 100 + 200 + 300 ms frames: about 4-7 frame changes in 1.3 s.
        check(3 <= len(paints) <= 20, "anim: image_animate paints the frames",
              "%d paints in 1.3 s" % len(paints))
    finally:
        U.stop(p)


def case_slide(exe, env, tmp):
    d = proj(tmp, "deck")
    body = (b"---\nmarp: true\n---\n\n# Title\n\n![bg left:40%](big.png)\n\nText beside.\n")
    p, log = launch_in(exe, env, d, "deck.md", body)
    try:
        win = window(env, p)
        if not win:
            print("skip: slide (no window)")
            return
        time.sleep(0.8)
        key(env, win, "shift+F5", settle=1.2)
        s = P.Shot(env, tmp, win, "slide")
        if not s.ok:
            print("skip: slide (no screenshot)")
            return
        whites = s.rows_where(P.white, 0.3)
        mid = (min(whites) + max(whites)) // 2 if whites else s.h // 2
        left = [s.at(x, mid) for x in range(0, int(s.w * 0.38), 4)]
        right = [s.at(x, mid) for x in range(int(s.w * 0.45), s.w, 4)]
        pic = sum(1 for c in left if any(P.near(c, k, 16) for k in (RED, GREEN, BLUE, (250, 250, 250))))
        check(pic > len(left) * 0.8, "slide: the bg picture fills the left 40%",
              "%d of %d samples" % (pic, len(left)))
        check(not any(P.near(c, RED, 16) or P.near(c, BLUE, 16) for c in right),
              "slide: the text side has no picture")
    finally:
        U.stop(p)


def case_viewer(exe, env, tmp):
    d = proj(tmp, "view")
    p, log = launch_in(exe, env, d, "big.png", None)
    try:
        win = window(env, p)
        if not win:
            print("skip: viewer (no window)")
            return
        time.sleep(1.2)
        a = P.Shot(env, tmp, win, "view_a")
        key(env, win, "ctrl+d", settle=1.0)
        b = P.Shot(env, tmp, win, "view_b")
        if not (a.ok and b.ok):
            print("skip: viewer (no screenshot)")
            return
        check(count(a, RED) > 5000, "viewer: an image file shows its picture", "%d red px" % count(a, RED))
        check(count(b, RED) < 50, "viewer: Ctrl-D shows its hex", "%d red px" % count(b, RED))
    finally:
        U.stop(p)


def case_browse(exe, env, tmp):
    d = proj(tmp, "browse")
    os.remove(os.path.join(d, "quad.png"))
    e = dict(env)
    e.update({"RTX_SAFE_HOME": os.path.join(tmp, "safe_browse"), "RTX_BLINK_IDLE_MS": "300"})
    p = subprocess.Popen([exe, d], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        win = window(env, p)
        if not win:
            print("skip: browse (no window)")
            return
        time.sleep(1.0)
        key(env, win, "Down", settle=1.2)
        s = P.Shot(env, tmp, win, "browse")
        if not s.ok:
            print("skip: browse (no screenshot)")
            return
        check(count(s, RED) > 800 and count(s, BLUE) > 800,
              "browse: an image file's preview is its picture", "%d red px" % count(s, RED))
    finally:
        U.stop(p)


def check_idle(tag, pid, secs=3.0):
    """Zero wakeups on every thread once work settles (U.idle_threads).
    The pinned ccc's sysmon ticks 50 times a second once any `@parallel`
    ran (the image lanes, browse): a note, a failure with
    RTX_IDLE_STRICT=1 (a runtime whose sysmon sleeps has no such tick)."""
    own, tick, detail = U.idle_threads(pid, secs)
    check(own == 0, "%s: idle, no thread wakes" % tag,
          "%d wakeups in %.1f s: %s" % (own, secs, detail))
    if tick:
        if U.IDLE_STRICT:
            check(False, "%s: idle, the runtime's sysmon sleeps" % tag,
                  "%d ticks in %.1f s: %s" % (tick, secs, detail))
        else:
            print("note: %s: ccc runtime sysmon ticked %d times in %.1f s "
                  "(needs the quiescent-sysmon runtime)" % (tag, tick, secs))


def case_idle_threads(exe, env, tmp):
    """Every thread of cctext-ui idle once work settles: a Markdown file
    with pictures, a workbook (a big one: its read job), a Marp deck
    presenting a picture slide, and browse with a preview open."""
    d = proj(tmp, "idle")
    shutil.copy(os.path.join(HERE, "..", "testdata", "wb", "revenue.wb.md"), d)
    with open(os.path.join(d, "big.wb.md"), "w") as f:
        f.write("# Big\n\nTable: T\n\n| id | amount | cost | margin |\n|----|----|----|----|\n")
        for i in range(60000):
            f.write("| %d | %d.50 | %d.25 | `=@amount - @cost` |\n" % (i, i % 997, i % 13))
        f.write("\n```calc\ntotal = sum(T.margin)\n```\n")
    md = (b"# Pictures\n\n![big one](big.png)\n\n![quad](quad.png)\n\nThe end.\n")
    deck = (b"---\nmarp: true\n---\n\n# Title\n\n![bg left:40%](big.png)\n\nText beside.\n")
    runs = [("markdown with pictures", "doc.md", md, ()),
            ("workbook", "revenue.wb.md", None, ()),
            ("big workbook (read job)", "big.wb.md", None, ()),
            ("Marp deck presenting", "deck.md", deck, ("shift+F5",)),
            ("browse preview", None, None, ("Down", "Down"))]
    for tag, name, body, keys in runs:
        if name:
            p, log = launch_in(exe, env, d, name, body)
        else:
            e = dict(env)
            e.update({"RTX_SAFE_HOME": os.path.join(tmp, "safe_idle"),
                      "RTX_BLINK_IDLE_MS": "300"})
            e.pop("RTX_UI_SCRIPT", None)
            p = subprocess.Popen([exe, d], env=e, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        try:
            win = window(env, p)
            if not win:
                print("skip: idle threads %s (no window)" % tag)
                continue
            time.sleep(0.8)
            if keys:
                key(env, win, *keys, settle=1.0)
            check_idle("idle threads: " + tag, p.pid)
        finally:
            U.stop(p)


class _Quiet(http.server.SimpleHTTPRequestHandler):
    hits = 0

    def log_message(self, *a):
        pass

    def do_GET(self):
        _Quiet.hits += 1
        return super().do_GET()


def case_remote(exe, env, tmp):
    d = proj(tmp, "remote")
    srvdir = os.path.join(tmp, "srv")
    os.makedirs(srvdir, exist_ok=True)
    shutil.copy(os.path.join(IMG, "big.png"), srvdir)

    def handler(*a, **k):
        return _Quiet(*a, directory=srvdir, **k)

    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    port = httpd.server_address[1]
    th = threading.Thread(target=httpd.serve_forever, daemon=True)
    th.start()
    body = b"![far](http://127.0.0.1:%d/big.png)\n\nafter\n" % port
    script = os.path.join(tmp, "remote.script")
    # 40 frames to lay out, a click on the placeholder (the caret starts on
    # the image's line, so its source is the first row and the placeholder
    # box sits under it), then the dialog's first button: "Load this image".
    with open(script, "w") as f:
        f.write("wait 40\nclick 200 64\ndlg 1\nwait 40\n")
    p, log = launch_in(exe, env, d, "doc.md", body, {"RTX_UI_SCRIPT": script})
    try:
        win = window(env, p)
        if not win:
            print("skip: remote (no window)")
            return
        time.sleep(0.4)
        before = _Quiet.hits
        pre = P.Shot(env, tmp, win, "remote_pre")
        time.sleep(3.5)
        post = P.Shot(env, tmp, win, "remote_post")
        lines = U.read_log(log)
        if os.environ.get("IMG_DEBUG"):
            print("\n".join(l for l in lines if not l.startswith("draw")))
        check(before == 0, "remote: nothing fetched before the click", "%d requests" % before)
        check(any("Remote image" in l for l in lines), "remote: the click asks (dialog)")
        check(_Quiet.hits == 1, "remote: one fetch after 'Load this image'",
              "%d requests" % _Quiet.hits)
        if post.ok:
            check(count(post, RED) > 2000, "remote: the picture arrives", "%d red px" % count(post, RED))
        if pre.ok:
            check(count(pre, RED) < 50, "remote: a placeholder before", "%d red px" % count(pre, RED))
    finally:
        U.stop(p)
        httpd.shutdown()


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
        with tempfile.TemporaryDirectory(prefix="cctext_img_") as tmp:
            for case in (case_markdown, case_anim, case_slide, case_viewer, case_browse,
                         case_remote, case_idle_threads):
                case(exe, env, tmp)
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
    print("%d failed" % len(FAILS))
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

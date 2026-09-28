#!/usr/bin/env python3
"""cctext-render protocol client (render/cr_proto.h): benchmarks and manual checks.

    python3 bench/render_client.py bench [BIN]          SVG: spawn-to-ready, warm per-SVG time, RSS
    python3 bench/render_client.py mermaid [BIN]        Mermaid: cold engine, warm per type, memory
    python3 bench/render_client.py leak [BIN] [JOBS]    Mermaid: RSS / heap over many jobs
    python3 bench/render_client.py math [BIN]           Math: cold engine, warm per formula, memory
    python3 bench/render_client.py png IN.svg OUT.png [SCALE]
    python3 bench/render_client.py mmd IN.mmd OUT.png [dark] [SCALE]
"""
import json
import os
import statistics
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# CrReq v2: magic id kind flags max_w scale em_px fg bg box_w box_h max_px len budget_ms node_max
REQ = struct.Struct('<IIBBHffIIIIIIII')
REP = struct.Struct('<IIII')
MAGIC_REQ, MAGIC_REP = 0x32515243, 0x31535243
K_SVG, K_TEX, K_MML, K_MERMAID, K_STATS = 1, 2, 3, 4, 5
R_HELLO, R_SIZE, R_PIXELS, R_ERROR, R_INFO = 0, 1, 2, 3, 4
MMD = os.path.join(ROOT, 'testdata', 'mermaid')
TYPES = ['flowchart', 'sequence', 'class', 'state', 'gantt', 'pie', 'er']


class Helper:
    def __init__(self, binary=None, args=(), env=None):
        binary = binary or os.path.join(ROOT, 'bin', 'cctext-render')
        t0 = time.perf_counter()
        self.p = subprocess.Popen([binary, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL if not os.environ.get('RTX_RENDER_DEBUG') else None,
                                  env=env if env is not None else {})
        typ, _id, payload = self._frame()
        assert typ == R_HELLO, typ
        self.hello = struct.unpack('<III', payload)
        self.ready_ms = (time.perf_counter() - t0) * 1000
        self.id = 0

    def _read(self, n):
        b = self.p.stdout.read(n)
        if len(b) != n:
            raise EOFError('helper died (rc=%s)' % self.p.poll())
        return b

    def _frame(self):
        magic, rid, typ, ln = REP.unpack(self._read(REP.size))
        assert magic == MAGIC_REP, hex(magic)
        return typ, rid, self._read(ln)

    def send(self, src, kind=K_SVG, flags=0, scale=1.0, box=(0, 0), max_px=0, bg=0, rid=None,
             budget_ms=0, node_max=0, em=16.0, fg=0x000000ff, max_w=0):
        if rid is None:
            self.id += 1
            rid = self.id
        b = src if isinstance(src, bytes) else src.encode()
        self.p.stdin.write(REQ.pack(MAGIC_REQ, rid, kind, flags, max_w, scale, em, fg, bg,
                                    box[0], box[1], max_px, len(b), budget_ms, node_max) + b)
        self.p.stdin.flush()
        return rid

    def render(self, src, size_only=False, flags=0, **kw):
        rid = self.send(src, flags=flags | (4 if size_only else 0), **kw)
        out = {}
        while True:
            typ, got, payload = self._frame()
            if got != rid:
                continue
            if typ == R_SIZE:
                out['w'], out['h'], out['baseline'], out['pw'], out['ph'] = struct.unpack('<fffII', payload)
                if size_only:
                    return out
            elif typ == R_PIXELS:
                pw, ph, stride = struct.unpack('<III', payload[:12])
                out['rgba'] = payload[12:]
                return out
            elif typ == R_ERROR:
                out['error'] = (struct.unpack('<I', payload[:4])[0], payload[4:].decode(errors='replace'))
                return out

    def mermaid(self, src, theme='default', size_only=False, scale=1.0, node_max=0, budget_ms=0,
                variables=None, **kw):
        opts = {'theme': theme}
        if variables:
            opts['themeVariables'] = variables
        payload = json.dumps(opts, separators=(',', ':')) + '\n' + src
        return self.render(payload, size_only=size_only, kind=K_MERMAID, scale=scale,
                           node_max=node_max, budget_ms=budget_ms, **kw)

    def math(self, src, mml=False, display=True, size_only=False, em=16.0, fg=0x000000ff,
             max_w=0, node_max=0, budget_ms=0, **kw):
        """TeX (or MathML) -> SIZE (w, h, baseline from the bottom) + PIXELS."""
        return self.render(src, size_only=size_only, kind=K_MML if mml else K_TEX,
                           flags=1 if display else 0, em=em, fg=fg, max_w=max_w,
                           node_max=node_max, budget_ms=budget_ms, **kw)

    def stats(self):
        rid = self.send(b'', kind=K_STATS)
        while True:
            typ, got, payload = self._frame()
            if got == rid and typ == R_INFO:
                return json.loads(payload.decode())

    def close(self):
        try:
            self.p.stdin.write(REQ.pack(MAGIC_REQ, 0, 0, 0, 0, 1.0, 16.0, 0, 0, 0, 0, 0, 0, 0, 0))
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        return self.p.wait()


def save_png(r, path):
    from PIL import Image
    im = Image.frombuffer('RGBA', (r['pw'], r['ph']), r['rgba'], 'raw', 'RGBa', 0, 1)
    im.save(path)


def rss_kb(pid):
    with open('/proc/%d/status' % pid) as f:
        for line in f:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])
    return 0


def bench(binary=None):
    med = statistics.median
    ready = []
    for _ in range(15):
        h = Helper(binary)
        ready.append(h.ready_ms)
        h.close()
    print('spawn -> ready (sandboxed): median %.1f ms, min %.1f ms (n=15)' % (med(ready), min(ready)))
    h = Helper(binary)
    print('RSS after hello: %.1f MB' % (rss_kb(h.p.pid) / 1024))
    import glob
    files = sorted(glob.glob(os.path.join(ROOT, 'testdata', 'svg', 'samples', '*.svg')))
    for f in files:
        src = open(f, 'rb').read()
        h.render(src)
        t = []
        for _ in range(10):
            t0 = time.perf_counter()
            r = h.render(src)
            t.append((time.perf_counter() - t0) * 1000)
        ts = []
        for _ in range(10):
            t0 = time.perf_counter()
            h.render(src, size_only=True)
            ts.append((time.perf_counter() - t0) * 1000)
        print('  %-28s %5dx%-5d size %6.2f ms  size+pixels %7.2f ms' % (
            os.path.basename(f), r.get('pw', 0), r.get('ph', 0), med(ts), med(t)))
    print('RSS after the samples: %.1f MB' % (rss_kb(h.p.pid) / 1024))
    h.close()


def bench_mermaid(binary=None, runs=6):
    """Cold engine start, warm time per type (distinct sources: the helper's
    SVG cache is bypassed), a size-only then pixels pair, memory."""
    med = statistics.median
    srcs = {t: open(os.path.join(MMD, t + '.mmd')).read() for t in TYPES}
    colds = []
    for k in range(3):
        h = Helper(binary)
        rss0 = rss_kb(h.p.pid)
        t0 = time.perf_counter()
        r = h.mermaid('pie\n  "a": %d\n  "b": 2\n' % (k + 1), size_only=True)
        colds.append((time.perf_counter() - t0) * 1000)
        st = h.stats()
        if k == 0:
            print('helper RSS after hello %.1f MB, after the first diagram %.1f MB; engine start %.0f ms, '
                  'bytecode inflate %.0f ms, heap %.1f MB' % (rss0 / 1024, rss_kb(h.p.pid) / 1024,
                                                              st['start_ms'], st['inflate_ms'], st['heap'] / 1048576))
        h.close()
    print('cold: first Mermaid request (engine start + a small pie): median %.0f ms %s' % (
        med(colds), ['%.0f' % c for c in colds]))
    h = Helper(binary)
    for t in TYPES:
        ts = []
        for k in range(runs):
            src = srcs[t] + '\n%%%% run %d\n' % k
            t0 = time.perf_counter()
            r = h.mermaid(src)
            ts.append((time.perf_counter() - t0) * 1000)
            if 'error' in r:
                print('  %-10s ERROR %r' % (t, r['error']))
                break
        tp = []
        for k in range(3):
            # the last source again: the helper's SVG cache, lunasvg only
            t0 = time.perf_counter()
            h.mermaid(srcs[t] + '\n%%%% run %d\n' % (runs - 1), box=(r['pw'], r['ph']))
            tp.append((time.perf_counter() - t0) * 1000)
        print('  %-10s %4dx%-4d first %6.0f ms  warm median %6.0f ms  (cached re-raster %5.1f ms)' % (
            t, r.get('pw', 0), r.get('ph', 0), ts[0], med(ts[1:]), med(tp)))
    st = h.stats()
    print('after the types: RSS %.1f MB, JS heap %.1f MB, GCs %d (%.0f ms)' % (
        rss_kb(h.p.pid) / 1024, st['heap'] / 1048576, st['gcs'], st['gc_ms']))
    h.close()


def bench_math(binary=None):
    """Cold engine start (first formula), warm time per formula over
    testdata/math/formulas.json (size + pixels, display, 16 px em), memory."""
    med = statistics.median
    forms = json.load(open(os.path.join(ROOT, 'testdata', 'math', 'formulas.json')))
    colds = []
    for k in range(5):
        h = Helper(binary)
        rss0 = rss_kb(h.p.pid)
        t0 = time.perf_counter()
        h.math('x^%d + 1' % (k + 2))
        colds.append((time.perf_counter() - t0) * 1000)
        st = h.stats()
        if k == 0:
            print('helper RSS after hello %.1f MB, after the first formula %.1f MB; engine start %.1f ms, '
                  'bytecode inflate %.1f ms, heap %.1f MB' % (
                      rss0 / 1024, rss_kb(h.p.pid) / 1024, st['math_start_ms'],
                      st['math_inflate_ms'], st['math_heap'] / 1048576))
        h.close()
    print('cold: first formula (engine start + x^2+1): median %.0f ms %s' % (
        med(colds), ['%.0f' % c for c in colds]))
    h = Helper(binary)
    first, warm, sizes = [], [], []
    for name, src in forms:
        t0 = time.perf_counter()
        r = h.math(src)
        first.append((time.perf_counter() - t0) * 1000)
        if 'error' in r:
            print('  %-12s ERROR %r' % (name, r['error']))
        ts, tz = [], []
        for k in range(3):
            v = src + ' {}' * (k + 1)  # distinct source: the helper's cache is bypassed
            t0 = time.perf_counter()
            h.math(v)
            ts.append((time.perf_counter() - t0) * 1000)
            t0 = time.perf_counter()
            h.math(v + '{}', size_only=True)
            tz.append((time.perf_counter() - t0) * 1000)
        warm.append(med(ts))
        sizes.append(med(tz))
    print('%d formulas: first use median %.1f ms (max %.0f ms, font files load on demand); '
          'warm size+pixels median %.1f ms (max %.1f); size only median %.1f ms' % (
              len(forms), med(first), max(first), med(warm), max(warm), med(sizes)))
    st = h.stats()
    print('after the formulas: RSS %.1f MB, math heap %.1f MB, source modules loaded %d' % (
        rss_kb(h.p.pid) / 1024, st['math_heap'] / 1048576, st['math_loads']))
    h.close()


def leak(binary=None, jobs=250):
    srcs = {t: open(os.path.join(MMD, t + '.mmd')).read() for t in TYPES}
    h = Helper(binary)
    t0 = time.time()
    errs = 0
    for i in range(jobs):
        t = TYPES[i % 7]
        r = h.mermaid(srcs[t] + '\n%%%% job %d\n' % i, size_only=True)
        errs += 'error' in r
        if i % 50 == 49:
            st = h.stats()
            print('  after %3d jobs: RSS %.1f MB, JS heap %.1f MB, starts %d, GCs %d' % (
                i + 1, rss_kb(h.p.pid) / 1024, st['heap'] / 1048576, st['starts'], st['gcs']))
    print('%d jobs in %.1f s, %d errors' % (jobs, time.time() - t0, errs))
    h.close()


if __name__ == '__main__':
    a = sys.argv
    if len(a) >= 2 and a[1] == 'bench':
        bench(a[2] if len(a) > 2 else None)
    elif len(a) >= 2 and a[1] == 'mermaid':
        bench_mermaid(a[2] if len(a) > 2 else None)
    elif len(a) >= 2 and a[1] == 'math':
        bench_math(a[2] if len(a) > 2 else None)
    elif len(a) >= 2 and a[1] == 'leak':
        leak(a[2] if len(a) > 2 and a[2] != '-' else None, int(a[3]) if len(a) > 3 else 250)
    elif len(a) >= 4 and a[1] == 'png':
        h = Helper()
        r = h.render(open(a[2], 'rb').read(), scale=float(a[4]) if len(a) > 4 else 1.0)
        if 'error' in r:
            sys.exit('error: %r' % (r['error'],))
        save_png(r, a[3])
        h.close()
    elif len(a) >= 4 and a[1] == 'mmd':
        h = Helper()
        rest = a[4:]
        r = h.mermaid(open(a[2]).read(), theme='dark' if 'dark' in rest else 'default',
                      scale=float(([x for x in rest if x != 'dark'] or ['1'])[0]))
        if 'error' in r:
            sys.exit('error: %r' % (r['error'],))
        save_png(r, a[3])
        h.close()
    else:
        sys.exit(__doc__)

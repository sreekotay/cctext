#!/usr/bin/env python3
"""cctext-render protocol client (render/cr_proto.h): benchmarks and manual checks.

    python3 bench/render_client.py bench [BIN]      spawn-to-ready, warm per-SVG time, RSS
    python3 bench/render_client.py png IN.svg OUT.png [SCALE]
"""
import os
import resource
import statistics
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REQ = struct.Struct('<IIBBHffIIIIII')
REP = struct.Struct('<IIII')
MAGIC_REQ, MAGIC_REP = 0x31515243, 0x31535243
R_HELLO, R_SIZE, R_PIXELS, R_ERROR = 0, 1, 2, 3


class Helper:
    def __init__(self, binary=None, args=(), env=None):
        binary = binary or os.path.join(ROOT, 'bin', 'cctext-render')
        t0 = time.perf_counter()
        self.p = subprocess.Popen([binary, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env=env)
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

    def send(self, src, kind=1, flags=0, scale=1.0, box=(0, 0), max_px=0, bg=0, rid=None):
        if rid is None:
            self.id += 1
            rid = self.id
        b = src if isinstance(src, bytes) else src.encode()
        self.p.stdin.write(REQ.pack(MAGIC_REQ, rid, kind, flags, 0, scale, 16.0, 0, bg,
                                    box[0], box[1], max_px, len(b)) + b)
        self.p.stdin.flush()
        return rid

    def render(self, src, size_only=False, **kw):
        rid = self.send(src, flags=4 if size_only else 0, **kw)
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

    def close(self):
        try:
            self.p.stdin.write(REQ.pack(MAGIC_REQ, 0, 0, 0, 0, 1.0, 16.0, 0, 0, 0, 0, 0, 0))
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


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'bench':
        bench(sys.argv[2] if len(sys.argv) > 2 else None)
    elif len(sys.argv) >= 4 and sys.argv[1] == 'png':
        h = Helper()
        r = h.render(open(sys.argv[2], 'rb').read(), scale=float(sys.argv[4]) if len(sys.argv) > 4 else 1.0)
        if 'error' in r:
            sys.exit('error: %r' % (r['error'],))
        save_png(r, sys.argv[3])
        h.close()
    else:
        sys.exit(__doc__)

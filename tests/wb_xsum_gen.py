#!/usr/bin/env python3
"""Generate testdata/wb/xsum.txt: exact-sum cases for core/wb_num (W2).

Every expected value is the exact rational sum rounded once to the
nearest double, ties to even (Python's Fraction -> float is correctly
rounded; OverflowError is +-inf), independent of order.

Lines (hex = the 16 hex digits of a double's bits):
  F <want> <n> <hex>...                     floats only
  M <want> <nf> <hex>... <nd> <v>:<sc>...   floats + decs (v / 10^sc)
  D <sc> <want|X> <nd> <v>:<sc>...          dec sum at scale sc (X: no int64)
  C <want> <v> <sc> <hex>                   sign(v / 10^sc - f)

  python3 tests/wb_xsum_gen.py > testdata/wb/xsum.txt
"""
import random
import struct
from fractions import Fraction

MAXD = struct.unpack('<d', struct.pack('<Q', 0x7FEFFFFFFFFFFFFF))[0]


def hx(x):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', x))[0]


def fromhex(h):
    return struct.unpack('<d', struct.pack('<Q', h))[0]


def rnd(q):
    """Fraction -> nearest double, ties to even; +-inf past the range."""
    if q == 0:
        return 0.0
    try:
        return float(q)
    except OverflowError:
        return float('inf') if q > 0 else float('-inf')


def exact(fs, ds=()):
    q = sum((Fraction(f) for f in fs), Fraction(0))
    q += sum((Fraction(v, 10 ** sc) for v, sc in ds), Fraction(0))
    return q


def fline(fs):
    return 'F %s %d %s' % (hx(rnd(exact(fs))), len(fs), ' '.join(hx(f) for f in fs))


def mline(fs, ds):
    return 'M %s %d %s %d %s' % (hx(rnd(exact(fs, ds))), len(fs), ' '.join(hx(f) for f in fs),
                                 len(ds), ' '.join('%d:%d' % d for d in ds))


def dline(sc, ds):
    q = exact((), ds) * 10 ** sc
    want = 'X'
    if q.denominator == 1 and -2 ** 63 <= q.numerator < 2 ** 63:
        want = str(q.numerator)
    return 'D %d %s %d %s' % (sc, want, len(ds), ' '.join('%d:%d' % d for d in ds))


def cline(v, sc, f):
    if f in (float('inf'), float('-inf')):
        want = -1 if f > 0 else 1
    else:
        q = Fraction(v, 10 ** sc) - Fraction(f)
        want = (q > 0) - (q < 0)
    return 'C %d %d %d %s' % (want, v, sc, hx(f))


def rand_double(r):
    kind = r.random()
    if kind < 0.1:
        return fromhex(r.getrandbits(52) | (r.choice([0, 1]) << 63))  # subnormal
    if kind < 0.3:
        e = r.randint(-1074, 1023)
    elif kind < 0.7:
        e = r.randint(-60, 60)
    else:
        e = r.randint(900, 1023)
    m = 1 + r.random()
    x = m * 2.0 ** min(e, 1023) if e > -1022 else fromhex(r.getrandbits(52))
    if x == float('inf'):
        x = MAXD
    return -x if r.random() < 0.5 else x


def main():
    r = random.Random(20260926)
    out = []
    cases = [
        [1e308, 1.0, -1e308],
        [1e308, 1e308, -1e308],
        [1e308, 1e308],
        [-1e308, -1e308],
        [MAXD, 2.0 ** 970],               # halfway to 2^1024: ties to even -> inf
        [MAXD, 2.0 ** 970, -2.0 ** 970],  # back to MAXD
        [MAXD, -MAXD],
        [5e-324] * 1000,
        [-5e-324] * 3,
        [1.0, 1e100, 1.0, -1e100],
        [1.0, 2.0 ** -53],                # tie: to even (1.0)
        [1.0, 2.0 ** -53, 2.0 ** -106],   # just above the tie: up
        [1.0 + 2.0 ** -52, 2.0 ** -53],   # tie: to even (up)
        [-0.0],
        [-0.0, 0.0],
        [0.1] * 10,
        [0.1, 0.2, 0.3, -0.6],
        [2.0 ** -1022, -5e-324],          # normal minus one subnormal ulp
        [2.0 ** -1022 - 5e-324 * 0, 2.0 ** -1022],
        [1e16, 1.0, -1e16],
        [3.0, 1e-300, -3.0],
        [],
    ]
    for c in cases:
        out.append(fline(c))
    # many tiny, cancellation, random wide sets (and their order shuffled)
    for n in (10, 100, 1000, 5000):
        fs = [rand_double(r) for _ in range(n)]
        out.append(fline(fs))
        big = [r.uniform(-1, 1) * 2.0 ** r.randint(-30, 30) for _ in range(n)]
        out.append(fline(big + [-x for x in big[: n // 2]]))
    for _ in range(40):
        fs = [rand_double(r) for _ in range(r.randint(1, 60))]
        out.append(fline(fs))
    # mixed float + dec
    mixed = [
        ([0.1], [(1, 1)]),
        ([1.0], [(1, 18)]),
        ([1e308, 1e308], [(-1, 0)]),
        ([2.0 ** -1074], [(0, 3)]),
        ([-0.5], [(5, 1)]),
        ([1e-20], [(123456789, 9), (-123456789, 9)]),
        ([], [(1, 1)]),
        ([3.5], [(9223372036854775807, 0), (9223372036854775807, 0)]),
    ]
    for fs, ds in mixed:
        out.append(mline(fs, ds))
    for _ in range(40):
        fs = [rand_double(r) if r.random() < 0.3 else r.uniform(-1e6, 1e6)
              for _ in range(r.randint(1, 20))]
        ds = [(r.randint(-2 ** 63, 2 ** 63 - 1) if r.random() < 0.2 else r.randint(-10 ** 9, 10 ** 9),
               r.randint(0, 18)) for _ in range(r.randint(1, 20))]
        out.append(mline(fs, ds))
    # exact dec sums
    decs = [
        (2, [(12000, 2), (30050, 2), (-1, 2)]),
        (0, [(9223372036854775807, 0), (1, 0)]),
        (0, [(9223372036854775807, 0), (1, 0), (-2, 0)]),
        (18, [(1, 18)] * 5),
        (3, [(1, 1), (22, 2), (333, 3)]),
        (1, [(-9223372036854775807 - 1, 0), (-1, 0)]),
        (0, []),
    ]
    for sc, ds in decs:
        out.append(dline(sc, ds))
    for _ in range(30):
        sc = r.randint(0, 18)
        ds = [(r.randint(-2 ** 63, 2 ** 63 - 1), r.randint(0, sc)) for _ in range(r.randint(1, 40))]
        out.append(dline(sc, ds))
    # exact dec / float comparison
    cmps = [
        (1, 1, 0.1), (3, 1, 0.3), (5, 1, 0.5), (0, 0, -0.0), (0, 0, 5e-324),
        (9007199254740993, 0, 9007199254740992.0), (-1, 18, -1e-18),
        (9223372036854775807, 0, 9.223372036854776e18), (1, 0, float('inf')),
        (1, 0, float('-inf')), (123, 2, 1.23),
    ]
    for v, sc, f in cmps:
        out.append(cline(v, sc, f))
    for _ in range(60):
        sc = r.randint(0, 18)
        v = r.randint(-10 ** 12, 10 ** 12)
        f = float(Fraction(v, 10 ** sc)) if r.random() < 0.5 else rand_double(r)
        out.append(cline(v, sc, f))
    print('\n'.join(out))


if __name__ == '__main__':
    main()

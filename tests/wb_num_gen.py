#!/usr/bin/env python3
"""Workbook numeric reference (docs/workbook.md "Numbers"): generate
testdata/wb/numops.wb.md and its expected dump testdata/wb/numops.expect
from an independent model of the contract — Python ints / Fractions for
the exact kinds, IEEE doubles for float — so the C engine is checked
against arithmetic it does not share.

The contract, as modelled here:
  * int and dec are exact: + - % at the larger scale, * at the sum of the
    scales; a result past 12 fraction digits is rounded ONCE, half away
    from zero, to 12; past int64 it is #num.
  * / is float division (each side converted to the nearest double).
  * a float on either side makes + - * % float (IEEE; % is C fmod, exact).
  * comparisons: exact for int / dec; a float against anything compares as
    doubles; NaN is unordered (= false, != true, < <= > >= false).
  * sum is exact over every row and rounded once (a float in it: to the
    nearest double, ties to even); NaN in it, or +inf with -inf: NaN.
  * min / max: NaN in the column makes the result NaN.
  * literals: -9223372036854775808 is an int; 9223372036854775808 alone is
    out of range (#parse).

    python3 tests/wb_num_gen.py        (rewrites the two testdata files)
"""
import math
import os
import random
import struct
from fractions import Fraction

I64_MAX = (1 << 63) - 1
I64_MIN = -(1 << 63)
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_MD = os.path.join(HERE, "..", "testdata", "wb", "numops.wb.md")
OUT_EXP = os.path.join(HERE, "..", "testdata", "wb", "numops.expect")


class Err(Exception):
    def __init__(self, tag):
        self.tag = tag


# values: ("int", i) | ("dec", i, sc) | ("float", f) | ("bool", b)
def dec(i, sc):
    return ("int", i) if sc == 0 else ("dec", i, sc)


def to_double(v):
    if v[0] == "float":
        return v[1]
    if v[0] == "dec":
        return float(v[1]) / float(10 ** v[2])
    return float(v[1])


def scale(v):
    return v[2] if v[0] == "dec" else 0


def fit(x, sc):
    if sc > 12:
        p = 10 ** (sc - 12)
        q, r = divmod(abs(x), p)
        if 2 * r >= p:
            q += 1
        x = q if x >= 0 else -q
        sc = 12
    if x > I64_MAX or x < I64_MIN:
        raise Err("#num")
    return dec(x, sc)


def trunc_rem(x, y):
    r = abs(x) % abs(y)
    return r if x >= 0 else -r


def arith(op, a, b):
    if op == "/":
        y = to_double(b)
        if y == 0:
            raise Err("#div0")
        return ("float", to_double(a) / y)
    if a[0] == "float" or b[0] == "float":
        x, y = to_double(a), to_double(b)
        if op == "+":
            return ("float", x + y)
        if op == "-":
            return ("float", x - y)
        if op == "*":
            return ("float", x * y)
        if y == 0:
            raise Err("#div0")
        if math.isnan(x) or math.isnan(y) or math.isinf(x):
            return ("float", float("nan"))
        return ("float", math.fmod(x, y))
    sa, sb = scale(a), scale(b)
    s = max(sa, sb)
    x = a[1] * 10 ** (s - sa)
    y = b[1] * 10 ** (s - sb)
    if op == "+":
        return fit(x + y, s)
    if op == "-":
        return fit(x - y, s)
    if op == "*":
        return fit(a[1] * b[1], sa + sb)
    if y == 0:
        raise Err("#div0")
    return fit(trunc_rem(x, y), s)


def compare(op, a, b):
    if a[0] == "float" or b[0] == "float":
        x, y = to_double(a), to_double(b)
        if math.isnan(x) or math.isnan(y):
            return ("bool", op == "!=")
        r = (x > y) - (x < y)
    else:
        sa, sb = scale(a), scale(b)
        s = max(sa, sb)
        x = a[1] * 10 ** (s - sa)
        y = b[1] * 10 ** (s - sb)
        r = (x > y) - (x < y)
    return ("bool", {"=": r == 0, "!=": r != 0, "<": r < 0, "<=": r <= 0, ">": r > 0,
                     ">=": r >= 0}[op])


def neg(a):
    if a[0] == "float":
        return ("float", -a[1])
    if a[1] == I64_MIN:
        raise Err("#num")
    return dec(-a[1], scale(a))


def c_trunc(x):
    if not (-4503599627370496.0 < x < 4503599627370496.0):
        return x
    return float(int(x))


def c_round(x):
    t = c_trunc(x)
    if x - t >= 0.5:
        return t + 1.0
    if t - x >= 0.5:
        return t - 1.0
    return t


def fn(name, a, nd=0):
    if name == "abs":
        if a[0] == "float":
            return ("float", abs(a[1]))
        if a[1] == I64_MIN:
            raise Err("#num")
        return dec(abs(a[1]), scale(a))
    if name == "round":
        if a[0] == "float":
            p = float(10 ** nd)
            return ("float", c_round(a[1] * p) / p)
        sc, x = scale(a), a[1]
        if nd >= sc:
            return fit(x * 10 ** (nd - sc), nd)
        p = 10 ** (sc - nd)
        q = abs(x) // p * (1 if x >= 0 else -1)
        r = trunc_rem(x, p)
        if r * 2 >= p:
            q += 1
        elif r * 2 <= -p:
            q -= 1
        return fit(q, nd)
    if name in ("floor", "ceil"):
        if a[0] == "float":
            t = c_trunc(a[1])
            if name == "floor":
                f = t - 1.0 if t > a[1] else t
            else:
                f = t + 1.0 if t < a[1] else t
            if -9.2e18 <= f <= 9.2e18:
                return ("int", int(f))
            return ("float", f)
        sc, x = scale(a), a[1]
        p = 10 ** sc
        q = abs(x) // p * (1 if x >= 0 else -1)
        r = trunc_rem(x, p)
        if name == "floor" and r < 0:
            q -= 1
        if name == "ceil" and r > 0:
            q += 1
        return ("int", q)
    raise ValueError(name)


def fmt(v):
    """The dump's text (rtx_wb_dump_ex with RTX_WB_DUMP_BITS)."""
    if v[0] == "int":
        return "%d" % v[1]
    if v[0] == "dec":
        i, sc = v[1], v[2]
        mag = abs(i)
        return "%s%d.%0*d" % ("-" if i < 0 else "", mag // 10 ** sc, sc, mag % 10 ** sc)
    if v[0] == "bool":
        return "true" if v[1] else "false"
    f = v[1]
    bits = struct.unpack("<Q", struct.pack("<d", f))[0]
    if math.isnan(f):
        t = "nan"
    elif math.isinf(f):
        t = "-inf" if f < 0 else "inf"
    else:
        t = "%.15g" % f
    return "%s [%016x]" % (t, bits)


def lit(v):
    """Formula text for a value (a parenthesised literal)."""
    if v[0] == "int":
        return "(%d)" % v[1]
    if v[0] == "dec":
        return "(%s)" % fmt(v)
    f = v[1]
    if math.isnan(f):
        return "(1e308 * 10 - 1e308 * 10)"
    if math.isinf(f):
        return "(1e308 * 10)" if f > 0 else "(-1e308 * 10)"
    return "(%.17e)" % f


def pool(rng):
    vals = [("int", 0), ("int", 1), ("int", -1), ("int", 7), ("int", -7), ("int", 3),
            ("int", I64_MAX), ("int", I64_MIN), ("int", I64_MAX - 1), ("int", 1 << 40),
            ("dec", 1, 1), ("dec", -5, 1), ("dec", 123456789012349, 14), ("dec", 5, 13),
            ("dec", -5, 13), ("dec", 15, 13), ("dec", 25, 13), ("dec", 999999999999999999, 18),
            ("dec", I64_MAX, 2), ("dec", I64_MIN, 2), ("dec", 1, 12), ("dec", 314159, 5),
            ("float", 0.1), ("float", -0.0), ("float", 1e308), ("float", -1e308),
            ("float", 5e-324), ("float", 1e20), ("float", 3.0), ("float", 2.5),
            ("float", -2.5), ("float", 1e-300), ("float", float("inf")),
            ("float", float("nan")), ("float", 9.2e18), ("float", 4503599627370497.0)]
    for _ in range(30):
        k = rng.randrange(3)
        if k == 0:
            vals.append(("int", rng.randrange(-(1 << 62), 1 << 62) >> rng.randrange(62)))
        elif k == 1:
            sc = rng.randrange(1, 19)
            vals.append(("dec", rng.randrange(-(1 << 62), 1 << 62) >> rng.randrange(62), sc))
        else:
            vals.append(("float", rng.choice([-1, 1]) * rng.random() * 10.0 ** rng.randrange(-30, 30)))
    return vals


def main():
    rng = random.Random(20260926)
    vals = pool(rng)
    lines = []   # (formula, expected text)

    def add(formula, thunk):
        try:
            v = thunk()
            want = fmt(v)
        except Err as e:
            want = e.tag
        lines.append((formula, want))

    ops = ["+", "-", "*", "/", "%"]
    cmps = ["=", "!=", "<", "<=", ">", ">="]
    # every op over a fixed grid of edge values, then random pairs
    edge = vals[:36]
    for a in edge[::3]:
        for b in edge[1::4]:
            for op in ops:
                add("%s %s %s" % (lit(a), op, lit(b)), lambda a=a, b=b, op=op: arith(op, a, b))
    for _ in range(500):
        a, b = rng.choice(vals), rng.choice(vals)
        op = rng.choice(ops + cmps)
        if op in cmps:
            add("%s %s %s" % (lit(a), op, lit(b)), lambda a=a, b=b, op=op: compare(op, a, b))
        else:
            add("%s %s %s" % (lit(a), op, lit(b)), lambda a=a, b=b, op=op: arith(op, a, b))
    for a in vals:
        add("-%s" % lit(a), lambda a=a: neg(a))
        add("abs(%s)" % lit(a), lambda a=a: fn("abs", a))
        add("floor(%s)" % lit(a), lambda a=a: fn("floor", a) if not (a[0] == "float" and (math.isnan(a[1]) or math.isinf(a[1]))) else a)
        add("ceil(%s)" % lit(a), lambda a=a: fn("ceil", a) if not (a[0] == "float" and (math.isnan(a[1]) or math.isinf(a[1]))) else a)
        for nd in (0, 2, 12):
            add("round(%s, %d)" % (lit(a), nd), lambda a=a, nd=nd: fn("round", a, nd))
    # named review cases
    add("1.23456789012349 * 1", lambda: arith("*", ("dec", 123456789012349, 14), ("int", 1)))
    add("1e20 % 3", lambda: arith("%", ("float", 1e20), ("int", 3)))
    add("-9223372036854775808", lambda: ("int", I64_MIN))
    add("-9223372036854775808 - 1", lambda: arith("-", ("int", I64_MIN), ("int", 1)))
    add("9223372036854775807 + 1", lambda: arith("+", ("int", I64_MAX), ("int", 1)))
    add("-92233720368547758.08", lambda: ("dec", I64_MIN, 2))
    add("(1e308 * 10 - 1e308 * 10) = (1e308 * 10 - 1e308 * 10)", lambda: ("bool", False))
    add("(1e308 * 10 - 1e308 * 10) != 1", lambda: ("bool", True))
    add("(1e308 * 10 - 1e308 * 10) < 1", lambda: ("bool", False))
    add("(1e308 * 10 - 1e308 * 10) >= 1", lambda: ("bool", False))
    add("min(1, 1e308 * 10 - 1e308 * 10, 2)", lambda: ("float", float("nan")))
    add("max(2, 1)", lambda: ("int", 2))
    lines.append(("9223372036854775808", "#parse"))

    # aggregates over table N (column x) — the review's sum, and friends
    col = [("float", 1e308), ("float", 1e308), ("float", -1e308)]
    tabs = [("N", "x", col)]
    col2 = [("dec", 1, 1), ("dec", 2, 1), ("float", 0.1), ("int", 3), ("dec", 5, 13)]
    tabs.append(("M", "x", col2))
    col3 = [("float", 1.0), ("float", float("nan")), ("float", 3.0)]
    tabs.append(("Q", "x", col3))
    col4 = [("int", I64_MAX), ("int", 1)]
    tabs.append(("O", "x", col4))
    col5 = [("dec", 123456789012345678, 18), ("dec", 1, 18), ("int", -1)]
    tabs.append(("P", "x", col5))

    def typed(c):
        """A column's literals under its type: any float makes every
        number float; else any dec rescales every number to the largest
        scale (a formula cell keeps its own value)."""
        if any(v[0] == "float" for v in c):
            return [("float", to_double(v)) for v in c]
        sc = max(scale(v) for v in c)
        return [dec(v[1] * 10 ** (sc - scale(v)), sc) for v in c]

    def xsum(c):
        c = typed(c)
        fl = [v[1] for v in c if v[0] == "float"]
        if fl:
            if any(math.isnan(f) for f in fl) or (float("inf") in fl and float("-inf") in fl):
                return ("float", float("nan"))
            if float("inf") in fl:
                return ("float", float("inf"))
            if float("-inf") in fl:
                return ("float", float("-inf"))
            tot = sum(Fraction(v[1]) if v[0] == "float" else Fraction(v[1], 10 ** scale(v)) for v in c)
            return ("float", float(tot))
        sc = max(scale(v) for v in c)
        tot = sum(v[1] * 10 ** (sc - scale(v)) for v in c)
        if tot > I64_MAX or tot < I64_MIN:
            raise Err("#num")
        return dec(tot, sc)

    def xavg(c):
        t = xsum(c)
        c = typed(c)
        return ("float", to_double(t) / float(len(c)))

    for name, cn, c in tabs:
        add("sum(%s.%s)" % (name, cn), lambda c=c: xsum(c))
        add("avg(%s.%s)" % (name, cn), lambda c=c: xavg(c))
        add("count(%s where %s > 0)" % (name, cn),
            lambda c=c: ("int", sum(1 for v in typed(c) if compare(">", v, ("int", 0))[1])))
    add("max(Q.x)", lambda: ("float", float("nan")))
    add("min(N.x)", lambda: ("float", -1e308))

    out = ["# Numbers", "",
           "Generated by tests/wb_num_gen.py: the numeric contract's reference", ""]
    for name, cn, c in tabs:
        out += ["Table: %s" % name, "", "| %s |" % cn, "|---|"]
        for v in c:
            if v[0] == "float" and math.isnan(v[1]):
                out.append("| =1e308 * 10 - 1e308 * 10 |")
            else:
                out.append("| %s |" % (fmt(v).split(" [")[0] if v[0] != "float" else "%.17e" % v[1]))
        out.append("")
    out += ["```calc"]
    exp = []
    for k, (formula, want) in enumerate(lines):
        out.append("r%d = %s" % (k, formula))
        exp.append("Numbers.r%d = %s" % (k, want))
    out += ["```", ""]
    with open(OUT_MD, "w") as f:
        f.write("\n".join(out))
    with open(OUT_EXP, "w") as f:
        f.write("\n".join(exp) + "\n")
    print("wb_num_gen: %d cases" % len(lines))


if __name__ == "__main__":
    main()

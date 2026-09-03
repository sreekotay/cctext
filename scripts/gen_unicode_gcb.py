#!/usr/bin/env python3
"""Generate UAX #29 grapheme-break tables for core/unicode/utf8_break_tab.cch.

Unicode 17.0.0 (pin UNICODE_VERSION). Prefers local files in --ucd-dir
(default: testdata/unicode), else downloads the pinned UCD URLs.

    python3 scripts/gen_unicode_gcb.py
    python3 scripts/gen_unicode_gcb.py --check   # tables + GraphemeBreakTest
"""
from __future__ import annotations

import argparse
import os
import sys
import urllib.request

UNICODE_VERSION = "17.0.0"
UCD = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd"

FILES = {
    "GraphemeBreakProperty.txt": f"{UCD}/auxiliary/GraphemeBreakProperty.txt",
    "GraphemeBreakTest.txt": f"{UCD}/auxiliary/GraphemeBreakTest.txt",
    "emoji-data.txt": f"{UCD}/emoji/emoji-data.txt",
    "DerivedCoreProperties.txt": f"{UCD}/DerivedCoreProperties.txt",
}

GCB = {
    "Other": 0,
    "CR": 1,
    "LF": 2,
    "Control": 3,
    "Extend": 4,
    "ZWJ": 5,
    "Regional_Indicator": 6,
    "Prepend": 7,
    "SpacingMark": 8,
    "L": 9,
    "V": 10,
    "T": 11,
    "LV": 12,
    "LVT": 13,
}
INCB_NONE, INCB_CONSONANT, INCB_LINKER, INCB_EXTEND = 0, 1, 2, 3
EP_BIT = 0x10
INCB_SHIFT = 5

GCB_OTHER = 0
GCB_CR = 1
GCB_LF = 2
GCB_CONTROL = 3
GCB_EXTEND = 4
GCB_ZWJ = 5
GCB_RI = 6
GCB_PREPEND = 7
GCB_SPACINGMARK = 8
GCB_L = 9
GCB_V = 10
GCB_T = 11
GCB_LV = 12
GCB_LVT = 13


def parse_range(field: str) -> tuple[int, int]:
    field = field.strip()
    if ".." in field:
        a, b = field.split("..", 1)
        return int(a, 16), int(b, 16)
    n = int(field, 16)
    return n, n


def parse_ucd_rows(text: str, prop_pred):
    """Yield (lo, hi, value) for non-comment data lines matching prop_pred."""
    for line in text.splitlines():
        raw = line.split("#", 1)[0].strip()
        if not raw:
            continue
        parts = [p.strip() for p in raw.split(";")]
        if len(parts) < 2:
            continue
        got = prop_pred(parts)
        if got is None:
            continue
        lo, hi = parse_range(parts[0])
        yield lo, hi, got


def load_text(path: str, url: str, allow_download: bool) -> str:
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            return f.read()
    if not allow_download:
        raise FileNotFoundError(f"missing {path} (pass --download)")
    print(f"download {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as r:
        data = r.read().decode("utf-8")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)
    return data


def collect_props(gbp: str, emoji: str, dcp: str) -> dict[int, int]:
    """Sparse map cp -> packed prop (only non-zero)."""
    m: dict[int, int] = {}

    def set_bits(lo: int, hi: int, mask: int, shift: int, val: int) -> None:
        for cp in range(lo, hi + 1):
            cur = m.get(cp, 0)
            cur &= ~mask
            cur |= (val << shift) & mask
            if cur:
                m[cp] = cur
            elif cp in m:
                del m[cp]

    for lo, hi, name in parse_ucd_rows(
        gbp, lambda p: p[1] if p[1] in GCB and p[1] != "Other" else None
    ):
        set_bits(lo, hi, 0x0F, 0, GCB[name])

    for lo, hi, _ in parse_ucd_rows(
        emoji, lambda p: True if p[1] == "Extended_Pictographic" else None
    ):
        for cp in range(lo, hi + 1):
            m[cp] = m.get(cp, 0) | EP_BIT

    incb_map = {"None": 0, "Consonant": 1, "Linker": 2, "Extend": 3}
    for lo, hi, name in parse_ucd_rows(
        dcp,
        lambda p: p[2] if len(p) >= 3 and p[1] == "InCB" and p[2] in incb_map else None,
    ):
        val = incb_map[name]
        if val == 0:
            continue
        for cp in range(lo, hi + 1):
            cur = m.get(cp, 0)
            cur = (cur & ~0x60) | (val << INCB_SHIFT)
            m[cp] = cur
    return m


def ranges_from_map(m: dict[int, int]) -> tuple[list[int], list[int]]:
    """Covering start[] / prop[] with Other gaps. start ends with 0x110000."""
    if not m:
        return [0, 0x110000], [0]
    cps = sorted(m)
    starts = [0]
    props = []
    # from 0 to first
    if cps[0] > 0:
        props.append(0)
        starts.append(cps[0])
    i = 0
    n = len(cps)
    while i < n:
        p = m[cps[i]]
        j = i + 1
        while j < n and cps[j] == cps[j - 1] + 1 and m[cps[j]] == p:
            j += 1
        # range cps[i] .. cps[j-1]
        if starts[-1] != cps[i]:
            # gap Other
            props.append(0)
            starts.append(cps[i])
        props.append(p)
        end = cps[j - 1] + 1
        starts.append(end)
        i = j
    if starts[-1] != 0x110000:
        props.append(0)
        starts.append(0x110000)
    # starts has one more than props
    assert len(starts) == len(props) + 1
    return starts, props


def lookup(starts: list[int], props: list[int], cp: int) -> int:
    if cp > 0x10FFFF:
        return 0
    lo, hi = 0, len(props) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= cp:
            lo = mid
        else:
            hi = mid - 1
    return props[lo]


def gcb(p: int) -> int:
    return p & 0x0F


def incb(p: int) -> int:
    return (p >> INCB_SHIFT) & 3


def is_ep(p: int) -> bool:
    return (p & EP_BIT) != 0


class St:
    __slots__ = ("gcb", "ri_odd", "emoji", "incb")

    def __init__(self) -> None:
        self.gcb = 0
        self.ri_odd = 0
        self.emoji = 0
        self.incb = 0


def feed(s: St, p: int) -> None:
    g = gcb(p)
    ic = incb(p)
    ep = is_ep(p)
    if g == GCB_RI:
        s.ri_odd = 0 if s.ri_odd else 1
    else:
        s.ri_odd = 0
    if ep:
        s.emoji = 1
    elif g == GCB_EXTEND and s.emoji == 1:
        s.emoji = 1
    elif g == GCB_ZWJ and s.emoji == 1:
        s.emoji = 2
    else:
        s.emoji = 0
    if ic == INCB_CONSONANT:
        s.incb = 1
    elif s.incb == 1 and ic == INCB_LINKER:
        s.incb = 2
    elif s.incb == 1 and ic == INCB_EXTEND:
        s.incb = 1
    elif s.incb == 2 and ic in (INCB_EXTEND, INCB_LINKER):
        s.incb = 2
    else:
        s.incb = 0
    s.gcb = g


def should_break(s: St, p: int) -> bool:
    prev = s.gcb
    curr = gcb(p)
    # GB3
    if prev == GCB_CR and curr == GCB_LF:
        return False
    # GB4
    if prev in (GCB_CR, GCB_LF, GCB_CONTROL):
        return True
    # GB5
    if curr in (GCB_CR, GCB_LF, GCB_CONTROL):
        return True
    # GB6
    if prev == GCB_L and curr in (GCB_L, GCB_V, GCB_LV, GCB_LVT):
        return False
    # GB7
    if prev in (GCB_LV, GCB_V) and curr in (GCB_V, GCB_T):
        return False
    # GB8
    if prev in (GCB_LVT, GCB_T) and curr == GCB_T:
        return False
    # GB9
    if curr in (GCB_EXTEND, GCB_ZWJ):
        return False
    # GB9a
    if curr == GCB_SPACINGMARK:
        return False
    # GB9b
    if prev == GCB_PREPEND:
        return False
    # GB9c
    if s.incb == 2 and incb(p) == INCB_CONSONANT:
        return False
    # GB11
    if s.emoji == 2 and is_ep(p):
        return False
    # GB12 / GB13
    if prev == GCB_RI and curr == GCB_RI and s.ri_odd:
        return False
    # GB999
    return True


def cluster_bounds(cps: list[int], starts: list[int], props: list[int]) -> list[int]:
    """Byte-less: return break-before indices, including 0 and len(cps)."""
    if not cps:
        return [0]
    bounds = [0]
    s = St()
    feed(s, lookup(starts, props, cps[0]))
    for i in range(1, len(cps)):
        p = lookup(starts, props, cps[i])
        if should_break(s, p):
            bounds.append(i)
            s = St()
        feed(s, p)
    bounds.append(len(cps))
    return bounds


def parse_gbt_line(line: str) -> tuple[list[int], list[bool]] | None:
    """Return (codepoints, break_before_each_including_start_and_end) or None."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    data = line.split("#", 1)[0].strip()
    toks = data.split()
    if not toks:
        return None
    cps: list[int] = []
    # tokens alternate marker, cp, marker, ..., marker
    # first should be ÷
    breaks: list[bool] = []
    expect_mark = True
    for t in toks:
        if expect_mark:
            if t == "÷":
                breaks.append(True)
            elif t == "×":
                breaks.append(False)
            else:
                return None
            expect_mark = False
        else:
            cps.append(int(t, 16))
            expect_mark = True
    if expect_mark:
        return None  # ended after a code point; missing final ÷
    # breaks has one more than cps (sot + between + eot)
    if len(breaks) != len(cps) + 1:
        return None
    return cps, breaks


def check_gbt(text: str, starts: list[int], props: list[int]) -> int:
    n = 0
    fail = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        parsed = parse_gbt_line(line)
        if parsed is None:
            continue
        cps, want = parsed
        n += 1
        bounds = cluster_bounds(cps, starts, props)
        got = [False] * (len(cps) + 1)
        for b in bounds:
            got[b] = True
        # sot/eot always break
        if got != want:
            fail += 1
            if fail <= 12:
                print(f"GBT fail line {lineno}: cps={[hex(c) for c in cps]}", file=sys.stderr)
                print(f"  want {want}", file=sys.stderr)
                print(f"  got  {got}", file=sys.stderr)
    if fail:
        raise SystemExit(f"GraphemeBreakTest: {fail}/{n} failed")
    print(f"GraphemeBreakTest: {n} lines ok", file=sys.stderr)
    return n


def emit_c(starts: list[int], props: list[int], n_gbt: int) -> str:
    lines = [
        "/* Generated by scripts/gen_unicode_gcb.py — do not edit. */",
        f"/* Unicode {UNICODE_VERSION} Grapheme_Cluster_Break + Extended_Pictographic + InCB. */",
        f"/* Covering ranges: {len(props)}. GraphemeBreakTest.txt: {n_gbt} cases. */",
        "#ifndef RTX_UTF8_BREAK_TAB_H",
        "#define RTX_UTF8_BREAK_TAB_H",
        "",
        "enum {",
        "    RTX_GCB_OTHER = 0,",
        "    RTX_GCB_CR = 1,",
        "    RTX_GCB_LF = 2,",
        "    RTX_GCB_CONTROL = 3,",
        "    RTX_GCB_EXTEND = 4,",
        "    RTX_GCB_ZWJ = 5,",
        "    RTX_GCB_RI = 6,",
        "    RTX_GCB_PREPEND = 7,",
        "    RTX_GCB_SPACINGMARK = 8,",
        "    RTX_GCB_L = 9,",
        "    RTX_GCB_V = 10,",
        "    RTX_GCB_T = 11,",
        "    RTX_GCB_LV = 12,",
        "    RTX_GCB_LVT = 13",
        "};",
        "",
        "#define RTX_UCD_EP 0x10u",
        "#define RTX_INCB_SHIFT 5",
        "#define RTX_INCB_CONSONANT 1",
        "#define RTX_INCB_LINKER 2",
        "#define RTX_INCB_EXTEND 3",
        "",
        f"#define RTX_UTF8_GCB_N {len(props)}",
        "",
        "static const uint32_t rtx_utf8_gcb_start[] = {",
    ]
    row: list[str] = []
    for i, v in enumerate(starts):
        row.append(f"0x{v:X}")
        if len(row) == 8:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines += [
        "};",
        "",
        "static const uint8_t rtx_utf8_gcb_prop[] = {",
    ]
    row = []
    for v in props:
        row.append(str(v))
        if len(row) == 16:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines += [
        "};",
        "",
        "#endif /* RTX_UTF8_BREAK_TAB_H */",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd-dir", default="testdata/unicode")
    ap.add_argument("--out", default="core/unicode/utf8_break_tab.cch")
    ap.add_argument("--download", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    ucd = args.ucd_dir
    os.makedirs(ucd, exist_ok=True)
    # Default: download if a required file is missing.
    allow = args.download or True
    texts = {}
    for name, url in FILES.items():
        texts[name] = load_text(os.path.join(ucd, name), url, allow)

    m = collect_props(
        texts["GraphemeBreakProperty.txt"],
        texts["emoji-data.txt"],
        texts["DerivedCoreProperties.txt"],
    )
    starts, props = ranges_from_map(m)
    print(
        f"Unicode {UNICODE_VERSION}: {len(m)} assigned cps, {len(props)} ranges, "
        f"table ~{4 * len(starts) + len(props)} bytes",
        file=sys.stderr,
    )
    n_gbt = check_gbt(texts["GraphemeBreakTest.txt"], starts, props)
    if args.check:
        return 0
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    body = emit_c(starts, props, n_gbt)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(body)
    print(f"wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

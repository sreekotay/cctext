#!/usr/bin/env python3
"""Make NotoSansSC-Regular.subset.ttf, cctext-render's CJK fallback face.

    python3 third_party/fonts/subset_cjk.py NotoSansSC_400Regular.ttf third_party/fonts/NotoSansSC-Regular.subset.ttf

Source: Noto Sans SC Regular, Google Fonts v40 static TTF (the file in the
npm package @expo-google-fonts/noto-sans-sc 0.4.3, 400Regular/; SHA-256 in
README.md). Needs fontTools (tested with 4.66.0). Deterministic: the same
source and fontTools give the same bytes.

Kept: the hanzi / kanji of GB 2312 (both levels) and JIS X 0208 (both
levels) -- 9,788 ideographs, nearly all modern Simplified Chinese and
Japanese text -- with CJK punctuation, kana, the Katakana extension,
kanbun, CJK radicals supplement and the full-width forms. Latin, Greek and
Cyrillic come from the Noto Sans faces; Hangul is not in Noto Sans SC.
Dropped: hinting (plutovg does not hint), BASE / STAT / vertical metrics,
layout features other than kern / palt / vert / vrt2 / locl.
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont


def double_byte(codec, lead, trail):
    out = set()
    for hi in range(lead[0], lead[1] + 1):
        for lo in range(trail[0], trail[1] + 1):
            try:
                s = bytes([hi, lo]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.add(ord(s))
    return out


def unicodes():
    gb = double_byte('gb2312', (0xB0, 0xF7), (0xA1, 0xFE))
    jis = double_byte('euc_jp', (0xB0, 0xF4), (0xA1, 0xFE))
    han = {c for c in gb | jis if 0x4E00 <= c <= 0x9FFF}
    extra = set()
    for a, b in [(0x2E80, 0x2EFF), (0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF),
                 (0x3190, 0x319F), (0x31F0, 0x31FF), (0xFF00, 0xFFEF)]:
        extra |= set(range(a, b + 1))
    return sorted(han | extra)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    opts = subset.Options()
    opts.layout_features = ['kern', 'palt', 'vert', 'vrt2', 'locl']
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    opts.hinting = False
    opts.drop_tables += ['BASE', 'STAT', 'vhea', 'vmtx', 'gasp', 'prep']
    font = TTFont(src, recalcTimestamp=False)
    s = subset.Subsetter(opts)
    s.populate(unicodes=unicodes())
    s.subset(font)
    font.save(dst)
    print(dst, len(font.getGlyphOrder()), 'glyphs')


if __name__ == '__main__':
    main()

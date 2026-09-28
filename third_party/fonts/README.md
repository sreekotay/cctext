# Fonts bundled with cctext-render

The only faces the SVG renderer knows (no system font discovery). They go
into `bin/cctext-render.pack` (zlib, read with Wuffs) at build time via
`render/manifest.txt`; `render/cr_svg.cpp` maps generic families
(`sans-serif`, `serif`, `monospace`, ...) and common named ones (Arial,
Helvetica, Times New Roman, Courier New, DejaVu Sans, Menlo, ...) onto them.

Licence: SIL Open Font License 1.1 (`OFL.txt`), Copyright 2022 The Noto
Project Authors; the CJK face: OFL 1.1 (`OFL-NotoSansSC.txt`), Copyright
2014-2021 Adobe, with Reserved Font Name 'Source' (not used by the subset,
which keeps the name Noto Sans SC).

Source: <https://github.com/notofonts/notofonts.github.io>, `fonts/<Family>/unhinted/ttf/`
(fetched 2026-09-27; unhinted: plutovg does not hint).

| File | Family | Version | SHA-256 |
|---|---|---|---|
| NotoSans-Regular.ttf | Noto Sans | 2.015 | f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8 |
| NotoSans-Bold.ttf | Noto Sans | 2.015 | 87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9 |
| NotoSans-Italic.ttf | Noto Sans | 2.015 | 678288f868807d4d64a6f3b51466871d117d915780381ce9d0ed4b3bcbd06d37 |
| NotoSerif-Regular.ttf | Noto Serif | 2.015 | a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1 |
| NotoSansMono-Regular.ttf | Noto Sans Mono | 2.014 | 87f8ce0522a6c99b743ee5fc75b4073cfdd575639119672828b7b9944b65b4f4 |
| NotoSansSC-Regular.subset.ttf | Noto Sans SC (subset) | 2.004 | e9286561c05e58da40be5e1817e23a2ca6f46c2cdd03f8dcad90878fe65bc97c |

The CJK face is a subset made by `subset_cjk.py` (fontTools 4.66.0,
deterministic) from Noto Sans SC Regular, Google Fonts v40 static TTF
(`NotoSansSC_400Regular.ttf` in the npm package
`@expo-google-fonts/noto-sans-sc` 0.4.3, SHA-256
d45f67f0a7c0ca3f256950777ce6a61cc7ce5f9696d02900cbbaac25f8aa7d16,
10.1 MiB): the 9,788 ideographs of GB 2312 and JIS X 0208 (both levels:
nearly all modern Simplified Chinese and Japanese), CJK punctuation, kana,
kanbun, the CJK radicals supplement and the full-width forms; no hinting.
3.2 MiB raw, 2.3 MiB in the pack. Simplified Chinese glyph shapes (a
Japanese or Traditional text draws its shared ideographs in SC style); no
Hangul (Noto Sans SC has none).

Coverage: Latin, Greek, Cyrillic (about 2,960 code points each; Noto Sans
Mono 3,490), and the CJK above. A glyph the chosen face lacks comes from
the first regular face that has it, in the manifest's order (Noto Sans,
Noto Serif, Noto Sans Mono, Noto Sans SC; plutovg patch 0005). No emoji,
no Hangul.

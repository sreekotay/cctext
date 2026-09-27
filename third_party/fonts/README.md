# Fonts bundled with cctext-render

The only faces the SVG renderer knows (no system font discovery). They go
into `bin/cctext-render.pack` (zlib, read with Wuffs) at build time via
`render/manifest.txt`; `render/cr_svg.cpp` maps generic families
(`sans-serif`, `serif`, `monospace`, ...) and common named ones (Arial,
Helvetica, Times New Roman, Courier New, DejaVu Sans, Menlo, ...) onto them.

Licence: SIL Open Font License 1.1 (`OFL.txt`), Copyright 2022 The Noto
Project Authors.

Source: <https://github.com/notofonts/notofonts.github.io>, `fonts/<Family>/unhinted/ttf/`
(fetched 2026-09-27; unhinted: plutovg does not hint).

| File | Family | Version | SHA-256 |
|---|---|---|---|
| NotoSans-Regular.ttf | Noto Sans | 2.015 | f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8 |
| NotoSans-Bold.ttf | Noto Sans | 2.015 | 87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9 |
| NotoSans-Italic.ttf | Noto Sans | 2.015 | 678288f868807d4d64a6f3b51466871d117d915780381ce9d0ed4b3bcbd06d37 |
| NotoSerif-Regular.ttf | Noto Serif | 2.015 | a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1 |
| NotoSansMono-Regular.ttf | Noto Sans Mono | 2.014 | 87f8ce0522a6c99b743ee5fc75b4073cfdd575639119672828b7b9944b65b4f4 |

Coverage: Latin, Greek, Cyrillic (about 2,960 code points each; Noto Sans
Mono 3,490). No CJK, no emoji, no per-glyph fallback between faces yet
(docs/images.md, "Renderer follow-ups"). Serif and mono have no bold or
italic face; lunasvg uses the regular one (no synthetic emboldening yet).

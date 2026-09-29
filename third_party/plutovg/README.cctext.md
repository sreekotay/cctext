# plutovg (vendored for cctext-render)

- Upstream: <https://github.com/sammycage/plutovg>
- Version: 1.3.3, the copy lunasvg `cf3594d5232e075fb344365191f0ecadd1519e1e`
  builds with (its `plutovg/` subdirectory)
- Licence: MIT (`LICENSE`). Bundled inside `source/`: FreeType-derived
  rasterizer and stroker (FreeType Licence, `source/FTL.TXT`), stb_image,
  stb_image_write and stb_truetype (public domain / MIT, in each header).
- Vendored: `include/`, `source/` only.

## cctext patches

Each with its description at the top (`patches/`), found by the
cctext-render fuzz run (ASan + UBSan):

1. `0001-clamp-coordinates.patch`: device coordinates clamped to +-2^20 px
   and stroke width / miter limit bounded before the fixed-point stroker
   and rasterizer (signed overflows), and no `memcpy` from a NULL border.
2. `0002-dash-and-flatten-bounds.patch`: over 1,000,000 dash segments in one
   stroke (a 1e-38 dash) draws it solid instead of overflowing the path
   array; a curve with non-finite flatness measures is drawn as its chord,
   subdivision stops at depth 16 and one flattening emits at most 2^20
   segments (a hostile arc took 3.3 s, a curve at FLT_MAX 16 s).
3. `0003-blend-conversions.patch`: texture offsets and fixed-point
   texture / gradient coordinates clamped before float -> integer
   conversion (a 1e38 translation computed `0 - INT_MIN`); the
   transformed blenders step in 64-bit.
4. `0004-kerning.patch`: text is laid out by one function shared by
   drawing and measuring, which adds the font's GPOS pair adjustment (or
   its kern table) between consecutive glyphs (stb_truetype);
   `plutovg_font_face_get_text_path` / `_traverse_text_path`.
5. `0005-glyph-fallback.patch`: a glyph the drawing face lacks comes from
   the first face registered with `plutovg_font_face_add_fallback` that
   has it (cctext-render: its regular faces in manifest order, the CJK
   face last); kerning only within one face.
6. `0006-synthetic-styles.patch`: a family without the bold / italic
   face asked for gets a synthetic variant of its closest face (a slant of
   tan 12 deg, an overstrike of size/24 .. size/32), created once and
   sharing its data; fallback glyphs get the style their own face lacks.
7. `0007-arc-nonfinite.patch`: an arc whose centre or sweep is not finite
   (a radius or endpoint at inf / NaN) is drawn as its chord instead of
   converting the NaN sweep to a segment count.

Build defines: `PLUTOVG_BUILD_STATIC`, `PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD`
(no system font scan), `STBI_MAX_DIMENSIONS=16384` (a `data:` image inside
an SVG is decoded by stb_image, inside the sandbox).

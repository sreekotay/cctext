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
   array; a curve with non-finite flatness measures is drawn as its chord
   (was 2^31 segments).

Build defines: `PLUTOVG_BUILD_STATIC`, `PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD`
(no system font scan), `STBI_MAX_DIMENSIONS=16384` (a `data:` image inside
an SVG is decoded by stb_image, inside the sandbox).

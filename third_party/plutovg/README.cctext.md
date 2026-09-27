# plutovg (vendored for cctext-render)

- Upstream: <https://github.com/sammycage/plutovg>
- Version: 1.3.3, the copy lunasvg `cf3594d5232e075fb344365191f0ecadd1519e1e`
  builds with (its `plutovg/` subdirectory)
- Licence: MIT (`LICENSE`). Bundled inside `source/`: FreeType-derived
  rasterizer and stroker (FreeType Licence, `source/FTL.TXT`), stb_image,
  stb_image_write and stb_truetype (public domain / MIT, in each header).
- Vendored: `include/`, `source/` only.

## cctext patches

`patches/0001-clamp-coordinates.patch` (description at its top): device
coordinates clamped to +-2^20 px and stroke width / miter limit bounded
before the fixed-point stroker and rasterizer (signed overflows the fuzz
run found), and no `memcpy` from a NULL border.

Build defines: `PLUTOVG_BUILD_STATIC`, `PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD`
(no system font scan), `STBI_MAX_DIMENSIONS=16384` (a `data:` image inside
an SVG is decoded by stb_image, inside the sandbox).

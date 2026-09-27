# lunasvg (vendored for cctext-render)

- Upstream: <https://github.com/sammycage/lunasvg>
- Pinned commit: `cf3594d5232e075fb344365191f0ecadd1519e1e` (master, lunasvg 3.5.0)
- Licence: MIT (`LICENSE`)
- Vendored: `include/`, `source/` only (no CMake / meson files; cctext
  builds it with `./make.shcc @cctext_render`, scripts/render_build.cch).
- Its dependency plutovg is vendored separately: `third_party/plutovg`.

## cctext patches

The tree is upstream plus these, in order (`patches/`, each with a
description at its top; `patch -p1 -d third_party/lunasvg < patches/000N-*.patch`
reapplies them onto a fresh upstream copy):

1. `0001-switch-conditional-processing.patch`: `<switch>`,
   `systemLanguage`, `requiredExtensions` (draw.io labels).
2. `0002-nesting-cap.patch`: element nesting capped at 256 levels (stack).
3. `0003-nested-svg-viewport.patch`: a nested `<svg>`'s percentage
   viewport resolved once per level (was 2^depth).
4. `0004-case-insensitive-font-family.patch`: font-family names match
   case-insensitively.

Build defines: `LUNASVG_BUILD_STATIC`, `LUNASVG_DISABLE_LOAD_SYSTEM_FONTS`,
`LUNASVG_DISABLE_EXTERNAL_RESOURCES` (only `data:` images load; an
`<image href="file:...">` or `http:` renders as missing).

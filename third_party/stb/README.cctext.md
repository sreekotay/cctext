# stb_image_resize2 (vendored for the picture resampler)

- Upstream: <https://github.com/nothings/stb> (Jeff Roberts, Jorge L
  Rodriguez, Sean Barrett and contributors)
- Version: stb_image_resize2 **v2.18** (2026-03-25), from the upstream
  repository at commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`
  (2026-08-01, the newest commit touching the file when vendored).
- Licence: public domain (Unlicense) or MIT, the reader's choice (the text
  is at the end of the header; `LICENSE` is upstream's repository file,
  the same text).
- Vendored, unmodified: `stb_image_resize2.h`, `LICENSE`.

  | file | SHA-256 |
  | --- | --- |
  | `stb_image_resize2.h` | `173e654634f6ccaad98f603e686ea212eec1fe8ea6d2a5e5e8056efa10ae3880` |
  | `LICENSE` | `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |

cctext compiles it in exactly one translation unit, `core/img_resize.c`,
with `STB_IMAGE_RESIZE_STATIC` (every symbol static) and `STBIR_ASSERT`
off (a failed internal check must not abort the editor); nothing else
includes it. It is used through its extended API: premultiplied BGRA
(`STBIR_BGRA_PM`), 8-bit values resampled as stored (sRGB, not
linearised), clamped edges, Catmull-Rom on both axes, the input sub-rect
after cctext's own box prefilter, and output split into bands so a
cancelled decode stops between them. SIMD: SSE2 on x86-64 and NEON on
arm64 come on by default; nothing else is configured. docs/images.md,
"Scaling".

To update: copy `stb_image_resize2.h` (and `LICENSE` if it changed) from
upstream, record the version, commit and hashes above, then build
(`./make.shcc @cctext_ui`) and run `img_smoke` (its resampler checks) and
`tests/ui_img_test.py`.

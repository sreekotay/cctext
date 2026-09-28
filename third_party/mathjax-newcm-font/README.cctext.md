# MathJax New Computer Modern font data (vendored for cctext-render)

- Upstream: <https://github.com/mathjax/MathJax-fonts>, npm package
  `@mathjax/mathjax-newcm-font` **4.1.3**, from
  `https://registry.npmjs.org/@mathjax/mathjax-newcm-font/-/mathjax-newcm-font-4.1.3.tgz`
  (npm integrity
  `sha512-gzAB3dFHilHX1l5x2xUqRL+1jDQt3Fyza1DkEMVXWC4E8SvsGdlgEza47HYi2WhVcgfkvf4zgUGzuhbq3Pjlew==`,
  checked). **Unmodified**: `svg.js` (the SVG output's base font) and the
  40 files of `svg/dynamic/` (glyph ranges loaded on demand: double-struck,
  fraktur, script, Greek, arrows, …), byte for byte from the tarball's
  `package/` directory.
- SHA-256 of `svg.js`:
  `74bd530bfc8944f8acecc14519e4623127b0014060a8544f075f3be31bfc6b0d`; the
  dynamic files' hashes are in `render/manifest.txt`.
- Licence: Apache-2.0 (the package's `license` field; the tarball ships no
  licence file, so `LICENSE` is the Apache-2.0 text from the `mathjax`
  package). The glyph outlines derive from the New Computer Modern fonts
  (GUST Font License); MathJax distributes this data under Apache-2.0.

`svg.js` is compiled into the math bundle's bytecode; the dynamic files
are packed as source and evaluated by the helper only when a formula uses
a glyph from one (their SHA-256 is checked first). See
`third_party/mathjax/README.cctext.md` and docs/images.md, "Math".

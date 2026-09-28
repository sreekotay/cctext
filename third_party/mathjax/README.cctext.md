# MathJax (vendored for cctext-render)

- Upstream: <https://github.com/mathjax/MathJax-src>, npm package
  `mathjax` **4.1.3**, from
  `https://registry.npmjs.org/mathjax/-/mathjax-4.1.3.tgz` (npm integrity
  `sha512-BN/8Pkgn7G1pIDYJqd9md+JHsE/jydSYbyOZnSdSA0WziuVO8mRxdYiWFumkVVly/8U+hm9DpIIoWuvySverzw==`,
  checked). **Every file is unmodified**, byte for byte from the tarball's
  `package/` directory.
- Licence: Apache-2.0 (`LICENSE`, the package's own).
- Files (SHA-256):

      13a303eae8790c5c79eddb2afdd3d22338be6fe59e0b526ebc92d00b88d98a1d  startup.js
      1a3cb32a18199355c67b8f78f57598dec8c253f7455ae42ac3f974b548d43a1b  core.js
      0375844c0c9698b3c26bde0c78b5bc62dbfbe15e3794433dcf7e0b4b3ea2b69a  input/tex.js
      ccb777a3698f3d2619876d7fdfc8d307a54896c3e1812f37aac24252687251ea  input/mml.js
      0027a9c7bef1a742868b00440d10d0be3648c3d570d722bb30206847b081d868  input/mml/entities.js
      6151d21b8205858c103642484f1ca272e3c9cbbde37d53235ac0572966ce613d  output/svg.js
      9b774a1859fea3d8829cd008d61321c5eb24c9553c3f4e0aaa47d86d83058490  adaptors/liteDOM.js

  and the 41 TeX extensions in `input/tex/extensions/`, whose SHA-256s
  are in `render/manifest.txt` (one line each).

The individual components are used, not the combined `tex-mml-svg.js`:
the combined file also carries the accessibility stack (the speech-rule
engine client, semantic enrichment, speech, the explorer) and the menu,
which a picture renderer never runs; without them the bytecode is 1.7 MB
instead of 13 MB, and nothing probes for Node globals.

The files run only inside the sandboxed helper `cctext-render`, in
QuickJS: `render/manifest.txt` wraps the core, the TeX and MathML inputs,
the SVG output, the liteDOM adaptor and the `begingroup` extension
unchanged as modules around `render/js/mj_shim.js` (configuration and the
loader's `require`), then `startup.js` and `render/js/mj_glue.js` (the
entry point), and compiles that to QuickJS bytecode; the other TeX
extensions go into the pack as source and are evaluated only when a
formula needs one (`\require`, or a macro the autoload extension maps to
one). The build checks every file's SHA-256 against the manifest and
refuses another; the helper checks the packed bytes against hashes
compiled into it before QuickJS sees them. No Node, npm or bundler is
involved. docs/images.md, "Math".

Not packed (the files stay here unmodified): `html`, `texhtml`,
`setoptions` (refused by `mj_shim.js`: links / raw HTML / parser options
from inside a formula) and `mhchem`, `bbm`, `bboldx`, `dsfont` (each
needs a MathJax font-extension package that is not vendored).

To update: download the new tarball, check its npm integrity, copy the
same files, regenerate the manifest's math lines (their SHA-256s) and the
hashes above, and run `math_smoke` (reference PNGs may move: regenerate
them with `testdata/math/gen_refs.py` after checking the differences).

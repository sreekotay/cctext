# Mermaid (vendored for cctext-render)

- Upstream: <https://github.com/mermaid-js/mermaid>, npm package
  `mermaid` **12.0.0**
- File: `mermaid.min.js`, the official IIFE dist file, byte for byte
  from `https://registry.npmjs.org/mermaid/-/mermaid-12.0.0.tgz`
  (`package/dist/mermaid.min.js`). **Unmodified.**
- SHA-256 of `mermaid.min.js`:
  `28fca7ae6ebc7ed7bb63bde63136a74bfef14f296a57e403657eeb8b32836073`
  (5 575 485 bytes). The tarball's npm integrity is
  `sha512-/wQXC9iBxoGV8p3erbvaXs9h77VyLDBH6GdayVjj3hEcSQhFU4N1WUhUppotCEqlIxI2pRMwjwBSwTB1MfZBgQ==`.
  `scripts/render_build.cch` checks the SHA-256 before every pack build
  and refuses a file that differs.
- Licence: MIT (`LICENSE`). The bundle carries its dependencies (d3,
  dagre-d3-es, cytoscape, elkjs, chevrotain / langium, DOMPurify,
  KaTeX, lodash-es, roughjs, …): their licences are in
  `THIRD_PARTY_LICENSES.txt` (MIT, ISC, BSD-3-Clause, Apache-2.0,
  MPL-2.0 OR Apache-2.0 for DOMPurify, EPL-2.0 for elkjs, Unlicense).

The file runs only inside the sandboxed helper `cctext-render`, in
QuickJS, over a handwritten DOM (`render/js/`): the build wraps it
unchanged between `render/js/rt_shim.js` + `render/js/mm_dom.js` and
`render/js/mm_glue.js`, compiles the lot to QuickJS bytecode and packs
it (zlib) with the fonts. No Node, npm or bundler is involved in the
build. docs/images.md, "Mermaid".

To update: download the new version's tarball, check its npm integrity,
copy `package/dist/mermaid.min.js` and `package/LICENSE`, regenerate
`THIRD_PARTY_LICENSES.txt` from the package's production dependency
tree, put the new SHA-256 here and in `scripts/render_build.cch`
(`RN_MERMAID_SHA256`), and run `mermaid_smoke` (the reference PNGs will
move).

## elkjs (EPL-2.0) inside the bundle

`mermaid.min.js` contains elkjs (Eclipse Layout Kernel, EPL-2.0), used
only by Mermaid's optional ELK layout; cctext never selects it (dagre
layouts only). EPL-2.0 is a weak, file-level copyleft: it covers the
elkjs code itself, not cctext or the rest of the bundle. We ship the
file unmodified with its licence text in `THIRD_PARTY_LICENSES.txt`.
Because the bundle is minified, its source for the elkjs portion is
available from upstream at <https://github.com/kieler/elkjs> (the
version pinned by mermaid 12.0.0's dependency tree) and from the npm
package `elkjs`; this notice is our pointer to that source. Modifying
the elkjs part of the file would oblige us to publish those changes
under EPL-2.0 — we don't.

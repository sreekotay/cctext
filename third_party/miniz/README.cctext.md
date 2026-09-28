# miniz (vendored for cctext-render's build step)

- Upstream: <https://github.com/richgel999/miniz> (Rich Geldreich, RAD Game
  Tools and Valve Software)
- Version: 3.1.0 (`MZ_VERSION` "11.3.0"), the release's amalgamated
  `miniz.c` / `miniz.h` (`miniz-3.1.0.zip` on the upstream releases page),
  fetched through the npm package `miniz.c@3.1.0`, which republishes that
  zip's two files unchanged.
- Licence: MIT (`LICENSE`, upstream's own file)
- Vendored, unmodified: `miniz.c`, `miniz.h`, `LICENSE`.

  | file | SHA-256 |
  | --- | --- |
  | `miniz.c` | `7487d4c8cd761b951a99d182672bb3badfa72a5ad5760b70b392fafa95223657` |
  | `miniz.h` | `13a6940a8f33b3a76d4b64fb6d482b246954bae3bf5dc713f7f054d2096d0aba` |
  | `LICENSE` | `0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3` |

cctext uses only its deflate encoder (tdefl), and only at build time:
`render/cr_pack.c` includes `miniz.c` with `MINIZ_NO_STDIO`,
`MINIZ_NO_TIME`, `MINIZ_NO_ARCHIVE_APIS`, `MINIZ_NO_ZLIB_APIS` and
`MINIZ_NO_INFLATE_APIS`, and `cctext-render --build-pack` compresses each
pack entry at level 9 (dynamic Huffman, lazy matching) into a zlib stream.
Nothing in cctext inflates with miniz: the helper reads the pack with
Wuffs (`third_party/wuffs`), and the build step round-trips every entry
through Wuffs before it writes the pack. docs/images.md, "Renderer".

To update: copy `miniz.c`, `miniz.h` and `LICENSE` from a new upstream
release zip, record the version and hashes above, and run
`./make.shcc @cctext_render` (it rebuilds the pack and its hash header)
and `@smoke`.

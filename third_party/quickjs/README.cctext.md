# QuickJS (vendored for cctext-render)

- Upstream: <https://github.com/bellard/quickjs> (Fabrice Bellard,
  Charlie Gordon)
- Pinned commit: `a38171d37357edff256c28a73eaeb41d2466d1d5` (`VERSION`
  2026-06-04)
- Licence: MIT (`LICENSE`)
- Vendored, unmodified: the engine only — `quickjs.c` / `.h`,
  `quickjs-atom.h`, `quickjs-opcode.h`, `libregexp.c` / `.h`,
  `libregexp-opcode.h`, `libunicode.c` / `.h`, `libunicode-table.h`,
  `cutils.c` / `.h`, `dtoa.c` / `.h`, `list.h`, `VERSION`, `LICENSE`.
  No `quickjs-libc` (no file, OS or thread bindings), no `qjs` / `qjsc`,
  no Makefile.

cctext builds it with `$CC` from `scripts/render_build.cch`
(`./make.shcc @cctext_render`; `-D_GNU_SOURCE -DCONFIG_VERSION=...`,
warnings off for this directory). The helper `render/cctext-render.c`
hosts it: `--build-pack` compiles the JavaScript to bytecode at build
time; the sandboxed helper loads that bytecode only from its own pack,
after checking a hash compiled into the binary (`JS_ReadObject` is not
hardened against hostile input). docs/images.md, "Mermaid".

To update: copy the same files from a new upstream commit, change the
commit above, rebuild (`@cctext_render` rebuilds the pack: bytecode is
tied to the engine build) and run `@smoke` (`mermaid_smoke`,
`render_fuzz`).

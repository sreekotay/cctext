# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
cctext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

`@typehooks` accepts only create / destroy / ufcs / ufcs_sink / niche.
Method names (`RtxBuf_type_cp`, `RtxWs_browse_start`) install by declaration.
Listing them in the hooks object still fails on the GUI TU (`unsupported
cc_type_register hook field`).

`frontend/gui.ccs` sits on the 8192 AST-node cap (one TU with the core).
Browse listing (`rtx_browse_kick` / pump / `>` walk) is a linked TU
(`core/browse.ccs`), same cut as hex. Browse `scanning` is the find `done`
bit inverted — same kick / step / yield (DESIGN.md Scan). Do not add a
shared `RtxScan` type. `#define` before `#include "….cch"` does not survive
into the generated C include — listing-only helpers belong in `browse.ccs`.
Workspace helpers (`RtxWs_browse_*`) live in `workspace.cch` so the listing
TU does not lower the hook table.

# Language friction (out-of-tree)

Concurrent-C recipes: `docs/cheatsheet.md` (Includes, UFCS, Keep).
DESIGN.md is product shape. This file is the include map: what cc
requires, then where cctext puts things. Recipes are `./make.shcc @`
(`ccc --as=shcc`). There is no Makefile.

## Concurrent-C

ccs features need a `.ccs`. Bodies, `!>(e) {`, dest-live `@parallel` —
put them in `foo.ccs` (or a face that file includes). Other TUs
`#include "foo.cch"` and link. They get decls.

Dest-live: pointer names copy the pointer; other locals are by reference
and must outlive `.wait()`. Do not assign a worker arm to a stack `int`.
`@serial { noop = 0; }` on the caller; the arm is an expression.
`@parallel wait` index loops use `for`, not `@for`.

## cctext

A `.cch` is the type plus the `RtxFoo_*` face (and the few `rtx_*`
other TUs call). Helpers stay in the owner `.ccs`.

Owned growable arrays are Vecs on their epoch arena (`vec_new` on plant;
unbind the handle before `reset` / `destroy`). `.len` is the extent
(read-only): grow with `push` / `reserve`, shrink with `truncate` /
`clear`. Hist text / ins are session `vec_from` wraps — assign a new
`from`, do not store `.len`. Leave raw + `cc_arena_realloc` where a Vec
grow would destroy live docs (`ws.bufs`), publish dest-live (`find.offs`,
`browse.ents`), or bump a session array of wraps (`hist.recs`). Field
UFCS on a Vec member peels as the owner type — bind `Vec *` then call.

`scratch_span(from, n, a)` — arena last; the caller names WHERE.
`RTX_FRAME_SCRATCH` is a stack budget, not a span cap.

Linked TUs (include the face, link the `.ccs`): find, browse, hex, grid,
nav, page_store, safe, scope, ui_help, workspace, layout, document.
Listing includes the browse face without `workspace.cch`. `RtxWs_browse_*`
stay workspace methods. `ui_types.cch` is included from `workspace.cch`
(same owner). `document.cch` is types plus the `RtxDoc_*` face.
`edit.cch` / `tm.cch` are chapter faces of that TU. Highlight has no
face; its bodies live in `document.ccs`. `nav.cch` is its own TU —
hosts that call `rtx_nav_*` include it.

Still textual (every host re-lowers them): `ui.cch`. Tree chapters, not TUs:
`piece_tree_rb.cch`, `piece_tree_lines.cch`, `piece_tree_priv.cch` —
included only by `piece_tree.ccs`. Brace-pair paint is `nav_pair.cch`
(linked callers), not `nav.cch`.

Where things live:
- Mark/fold bodies: `core/nav.ccs`; `nav.cch` is the `rtx_nav_*` face.
- Find: `core/find.ccs` (dest-live wrapper; wait-for tickets; tests call
  `find_finish`).
- Island dest-live: `core/piece_tree.ccs` (wait-for / `rtx_line_isle_finish`).
  `isle_kick` in `piece_tree_lines.cch` plants `listing_start`. Tests that
  wait call `finish`.
- Page store: `core/page_store.ccs`.
- Hex geometry: `core/hex.ccs`.
- Scope intern: `core/scope.ccs` (paint and lex share IDs).
- Help strings: `core/ui_help.ccs`.
- Safe journals: `core/safe.ccs`.
- Workspace: `core/workspace.ccs`.
- Layout: `core/layout.ccs`.
- Document: `core/document.ccs` (edit / highlight / tm bodies).
- Browse listing: `core/browse.ccs`. Grid geometry: `core/grid.ccs`
  (`rtx_grid_col_hi` so TUI and GUI do not each invent field.hi).
  Listing is dest-live + wait-for dir jobs; write stage appends ents;
  tests call `finish`.
- TUI: draw `cctext_draw.ccs`, grid paint `cctext_grid_draw.ccs`,
  keys `cctext_input.ccs`, Darwin session-wheel `cctext_osx.ccs`,
  host loop `cctext.ccs`.
- GUI: paint `gui_draw.ccs`, keys `gui_input.ccs`, host `gui.ccs`.
  Chrome `gui_chrome.ccs` / `gui_chrome.cch`. `objc_msgSend` (file picker)
  `gui_osx.ccs`. Platform window + Core Text is `frontend/gui_plat.m`.

Browse `scanning` is the chrome bit (handle live, not yet done). Find,
island, and browse plant with `find_set` / `isle_kick` / `rtx_browse_kick`;
tests that wait call `finish`. Do not add a shared `RtxScan` type. Vis-row motion is
`rtx_layout_soft_wrap`; do not gate Home/End on `L.wrap`. The live
workspace is the panes; parked files are paths + on-disk hist, not N
piece trees.

# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
cctext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

`@typehooks` is lifecycle only (create / destroy / ufcs / ufcs_sink / niche).
Methods are `Type_name` declarations, not hook fields. see Concurrent-C 
`docs/typehooks-typeviews.md`

`frontend/gui.ccs` and `frontend/cctext.ccs` sit on the 32768 AST-node cap
(one TU with the core). Mark/fold bodies live in `core/nav.ccs` (linked TU,
same cut as hex / browse / grid); `nav.cch` is only decls plus thin wrappers.
Find refine (`rtx_find_refine`) is `core/find.ccs` so prefix-extend does not
sit on the cctext include TU.
The scope intern table is `core/scope.ccs` — paint and lex must share IDs
(GUI draw is a separate TU). Help strings are `core/ui_help.ccs`.
Brace-pair paint (`rtx_nav_pair_ends`) is declared in `nav_pair.cch`, not
`nav.cch`. TUI pane + chrome paint is `frontend/cctext_draw.ccs` (same cut as
`gui_draw`); grid pane paint is `frontend/cctext_grid_draw.ccs` so stock `ccc`
(8192 AST) can still lower `cctext_draw`. Keys/mouse are `frontend/cctext_input.ccs`;
the host loop stays in `cctext.ccs`. Darwin session-wheel (Cursor drops Shift+wheel
`deltaX`) is `frontend/cctext_osx.ccs`. Find/browse chapter bodies live in the sibling
`.ccs` (CCC will not keep impl-grade statics in a header included from two TUs).
GUI paint is `frontend/gui_draw.ccs`; keys/mouse are `frontend/gui_input.ccs`;
the host loop stays in `frontend/gui.ccs` (same cut as hex / browse). Platform
window + Core Text is `frontend/gui_plat.m` (clang object linked into
`cctext-gui`). Do not add `@typehooks` fields on the GUI TUs. Chrome (menus /
fonts / help) is `frontend/gui_chrome.ccs`; `objc_msgSend` (file picker) is
`frontend/gui_osx.ccs`.
Safe journals (`rtx_safe_flush` / load / park) are a linked TU
(`core/safe.ccs`), same cut as browse. The live workspace is the panes;
parked files are paths + on-disk hist, not N piece trees.
Browse listing (`rtx_browse_kick` / pump / `>` walk) is a linked TU
(`core/browse.ccs`), same cut as hex / grid. Grid geometry is `core/grid.ccs` (`rtx_grid_col_hi` lives there so TUI
and GUI paint cannot each invent field.hi). Browse spawn (self / sibling frontend)
lives there too. Vis-row motion is `rtx_layout_soft_wrap`; do not gate
Home/End on `L.wrap`. `scratch_span(from, n, a)` — arena last; the
caller names WHERE. `RTX_FRAME_SCRATCH` is a stack budget, not a span cap. Browse `scanning` is the find `done` bit inverted — same
kick / step / yield (DESIGN.md Scan). Do not add a shared `RtxScan` type.
`#define` before `#include "….cch"` does not survive into the generated C
include — listing-only helpers belong in `browse.ccs`.
Workspace helpers (`RtxWs_browse_*`) live in `workspace.cch` so the listing
TU does not lower the hook table. 
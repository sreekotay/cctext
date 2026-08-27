# Language friction (out-of-tree)

Concurrent-C limits that decide where cctext code lives. Language recipes:
Concurrent-C `docs/cheatsheet.md` (Includes, UFCS, Keep). DESIGN.md is
product shape; this file is the include map.

A `.cch` next to a `.ccs` extracts as decls in every other TU. A file-scope
table or function body in a `.cch` two TUs include is two copies (or a
harvest blank) — chapter bodies go in the sibling `.ccs` (find, browse, hex,
grid, nav, safe, scope, ui_help). A `#define` before `#include "foo.cch"`
does not decide `#ifdef` inside `foo.cch` once that face extracts to `.h`;
do not gate listing helpers that way. Include only the faces whose lowered
meaning this TU owns — `RtxWs_browse_*` stay in `workspace.cch` so the
listing TU does not lower `RtxWs` / `RtxBuf`.

`scratch_span(from, n, a)` — arena last; the caller names WHERE.
`RTX_FRAME_SCRATCH` is a stack budget, not a span cap.

Where things live:
- Mark/fold bodies: `core/nav.ccs`; `nav.cch` is decls plus thin wrappers.
- Brace-pair paint: `nav_pair.cch` (linked callers), not `nav.cch`.
- Find: `core/find.ccs`. Island dest-live lives in the piece-tree TU
  (`isle_kick` plants; tests call `finish`).
- Scope intern: `core/scope.ccs` (paint and lex share IDs).
- Help strings: `core/ui_help.ccs`.
- Safe journals: `core/safe.ccs`.
- Browse listing: `core/browse.ccs`. Grid geometry: `core/grid.ccs`
  (`rtx_grid_col_hi` lives there so TUI and GUI cannot each invent field.hi).
  Listing is a dest-live `@parallel`: pause only around paint; kick
  and drop cancel+wait; tests call `finish` (wait). `RtxWs_browse_*`
  stay in `workspace.cch`.
- TUI: draw `cctext_draw.ccs`, grid paint `cctext_grid_draw.ccs`,
  keys `cctext_input.ccs`, Darwin session-wheel `cctext_osx.ccs`,
  host loop `cctext.ccs`.
- GUI: paint `gui_draw.ccs`, keys `gui_input.ccs`, host `gui.ccs`.
  Chrome `gui_chrome.ccs`. `objc_msgSend` (file picker) `gui_osx.ccs`.
  Platform window + Core Text is `frontend/gui_plat.m`.

Browse `scanning` is the chrome bit (handle live, not yet done). Find
and island are dest-live too: `find_set` / `isle_kick` plant, tests
call `finish` (wait). Worker arms are expressions so they do not write
a stack `noop`. Pointer names copy; other captures must outlive
`.wait()`. Do not add a shared `RtxScan` type. Vis-row motion is `rtx_layout_soft_wrap`; do not gate Home/End on
`L.wrap`. The live workspace is the panes; parked files are paths +
on-disk hist, not N piece trees.

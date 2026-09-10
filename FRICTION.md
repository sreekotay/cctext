# Language friction (out-of-tree)

Current cc / cctext landmines. DESIGN.md is product shape. Recipes:
`./make.shcc @` (`ccc --as=shcc`). Concurrent-C recipes: `docs/cheatsheet.md`.

## Concurrent-C

A face with non-`static` bodies has one owner `.ccs` (same stem, or a
same-directory include). Other TUs `#include "foo.cch"` and link the
owner; they get decls. Dest-live `@parallel` stays `.ccs`-only.
Statement `!>(e) {` in extractable `static inline` rewrites in the
lowered `.h` and does not force a splice.

Dest-live: pointer names copy the pointer; other locals are by reference
and must outlive `.wait()`. Do not assign a worker arm to a stack `int`
(`noop = arm()`): that is still `&noop` after kick returns. `@serial { noop
= 0; }` on the caller; the arm is an expression. `@parallel wait` index
loops use `for`, not `@for`. `.wait()` does not unpause — resume first
if a write-stage honor is parked.

`v->len - n` as a standalone expression is a shrink (`.len` is
read-only). Copy `.len` into a local first. `v->data[v->len - 1]` as
an index is fine.

Destroy / invalidate join with `cc__parallel_cancel_tree` +
`cc_parallel_join`, not UFCS `h.wait() !>`.

## cctext

Do not Vec `ws.bufs`, `find.offs`, `browse.ents`, or `hist.recs` — grow
would destroy live docs, publish dest-live, or drop session wraps. Those
stay raw + `cc_arena_realloc` (`ws.bufs` on `w->session`; browse hold on
`hold_a`). Browse walk (`RtxBrowseWalk`) stays `calloc` — it embeds
`CCParallel` and must not live in a bump arena. Hist text / ins are session
`vec_from` wraps: assign a new `from`, do not store `.len`. Find lane
scratch (block window + hit offs) lives on a per-lane heap arena in the
`@parallel` cache replica; island / browse job scratch / nav / line-index
splice / save pin / MD col rewrite / TUI OS-clipboard read scratch are
short-lived heap arenas (not malloc) — leaving arena / dest-live surfaces
for malloc is a regression unless the scratch dies with its arm. Host ObjC
clipboard cache in `gui_plat.m` stays process-local `realloc` (outside CC
arenas).
`RtxBrowse` is duplicated under `RTX_BROWSE_TYPES` in `workspace.cch`
(lowering needs the enums before `RtxBrowsePrev`); keep that copy in
lockstep with `browse.cch` — a shorter layout silently wins and
misaligns `hold_a`.

`ui.cch` is owned by `ui.ccs` (`rtx_ui`). `ui_types.cch` is decls;
gutter / rail / blink bodies live in `workspace.ccs`. Tree chapters
(`piece_tree_rb.cch`, `piece_tree_lines.cch`, `piece_tree_priv.cch`)
are not TUs — include them only from `piece_tree.ccs`. Pair-join edit
paths go through `rtx_buf_pair_replace` (Rich pane); `RtxDoc_replace_join`
is markup policy on top of one `replace`, not a second write.

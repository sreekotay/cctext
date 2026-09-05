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
stay raw + `cc_arena_realloc`. Hist text / ins are session `vec_from`
wraps: assign a new `from`, do not store `.len`.

`ui.cch` is owned by `ui.ccs` (`rtx_ui`). `ui_types.cch` is decls;
gutter / rail / blink bodies live in `workspace.ccs`. Tree chapters
(`piece_tree_rb.cch`, `piece_tree_lines.cch`, `piece_tree_priv.cch`)
are not TUs — include them only from `piece_tree.ccs`.

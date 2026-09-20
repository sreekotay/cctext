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

Destroy / drop: resume if paused, then `h.invalidate()` (cancels the
tree, then joins). `.wait()` does not cancel adopted children.

`@switch` `case .arm(bind):` with a `Vec::[T]` payload mis-lowers (splits
the function). Use bare `case .arm:` and a dominated `v.arm` projection.
`.pieces(refs)`-style struct binds are fine. Prefer `@switch` over
`arm ?> …` when the subject is behind `const` (const temporary assign).

## cctext

Do not Vec `hist.recs` — rows hold owned session `Vec::[char]` payloads;
outer-table element destroy/assign can drop those. `hist.recs` stays raw +
`cc_arena_realloc`. `find.hits` / `browse.ents` are `Vec::[T]` on their
epoch arena (append in `@stage`; do not copy the Vec handle across a
grow). `browse.hold` stays raw on `hold_a` (survives `store.reset`).
`ws.bufs` is a table of
`RtxBuf *`; each buffer is allocated once, so growing the table does not
move a document a find or island already holds. Browse walk (`RtxBrowseWalk`) stays `calloc` — it embeds
`CCParallel` and must not live in a bump arena. Hist text / ins are owned
session `Vec::[char]`: grow with `reserve` / `at_grow` in place; never
copy a handle into a same-arm `@variant` assign (destroy releases the
live owner). Find lane
scratch (block window + hit offs) lives on a per-lane heap arena in the
`@parallel` cache replica; island / browse job scratch / nav / line-index
splice / save pin / MD col rewrite / TUI OS-clipboard read scratch are
short-lived heap arenas (not malloc) — leaving arena / dest-live surfaces
for malloc is a regression unless the scratch dies with its arm. Host ObjC
clipboard cache in `gui_plat.m` stays process-local `realloc` (outside CC
arenas).
CCC nursery worker-frees: wait must not `free` until the last child's
`wake_all` finishes (`wake_published` handoff in `cc/runtime/nursery.c`).
Without that, `@smoke_asan` hits `heap-use-after-free` in
`wake_primitive_wake_all` from browse/isle `@parallel wait`. Install a
CCC build that includes the handoff. Remaining ASan `unknown-crash` on
exclusive/turnstile wake under fibers is the usual Darwin fiber-stack
false positive (see concurrent-c `docs/sanitizers.md`).

`ui.cch` is owned by `ui.ccs` (`rtx_ui`). `ui_types.cch` is decls;
gutter / rail / blink bodies live in `workspace.ccs`. Tree chapters
(`piece_tree_rb.cch`, `piece_tree_lines.cch`, `piece_tree_priv.cch`)
are not TUs — include them only from `piece_tree.ccs`. Pair-join edit
paths go through `rtx_buf_pair_replace` (Rich pane); `RtxDoc_replace_join`
is markup policy on top of one `replace`, not a second write.

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

An expression-position `!>(e) { … }` body must diverge: `continue` /
`break` do not count (`body must diverge (return, goto, @err, @ok, …)`).
In a loop, wrap the Result call in a helper that returns NULL / 0 on
error (`try_compile` in tests/rx_smoke.ccs) and test that.

`@switch` `case .arm(bind):` with a `Vec::[T]` payload mis-lowers (splits
the function). Use bare `case .arm:` and a dominated `v.arm` projection.
`.pieces(refs)`-style struct binds are fine. Prefer `@switch` over
`arm ?> …` when the subject is behind `const` (const temporary assign).
`@for` over a Vec in a header `static inline` has mis-typed sibling
TUs (ambiguous `as:` embed); keep those walks in the `.ccs`.

## cctext

Do not Vec `hist.recs` — rows hold owned session `Vec::[char]` payloads;
outer-table element destroy/assign can drop those. `hist.recs` stays raw +
`cc_arena_realloc`. `find.hits` / `browse.ents` are `Vec::[T]` on their
epoch arena (append in `@stage`; do not copy the Vec handle across a
grow). Prefer `@for (h in hits)` / `@for (i, h in hits)` for read walks
(`recipe_walk`); keep indexed form for compact / random `cur` / pointer
mut. `browse.hold` stays raw on `hold_a` (survives `store.reset`).
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
for malloc is a regression unless the scratch dies with its arm. Host
clipboard cache in `ui_plat.c` stays process-local `malloc` from the
`ui_os_*` shim (outside CC arenas).
CCC nursery worker-frees: wait must not `free` until the last child's
`wake_all` finishes (`wake_published` handoff in `cc/runtime/nursery.c`).
Without that, `@smoke_asan` hits `heap-use-after-free` in
`wake_primitive_wake_all` from browse/isle `@parallel wait`. Install a
CCC build that includes the handoff.

CCC turnstile cond wake writes a returned stack frame (concurrent-c
`0a633969`, `cc/runtime/exclusive.c` `cc__exclusive_cond_wake_n`): it
stores `w->ready = 1`, then `wake_primitive_wake_one(&w->wake)`. `w` is
the waiter's stack node; once `ready` is seen the waiter returns, so the
`fetch_add` lands in whatever frame reuses that stack — usually the
next `cc_exclusive_mutex_acquire`'s `CCExclusiveMutex m`, turning
`m._entry` into `ptr + 2^32`. Linux `@smoke_asan` reports it as
`stack-buffer-overflow` in `wake_primitive_wake_one` from any `@stage`
(browse `rtx_browse_run`, find `rtx_find_run`); `@smoke_tsan`
edit_session_smoke SEGVs in `cc_exclusive_mutex_acquire` and hangs
2 runs in 3. Not a cctext bug: bump `wake.value` before `ready` for an
OS-thread waiter and touch nothing after `ready` for a fiber waiter
(`cc_exclusive_unlock_contended` already does the latter). With that
patch every smoke is ASan- and TSan-clean. `scripts/tsan.supp` lists the
remaining runtime-only TSan reports (gate cell retire, lazy init); a
report with a cctext frame is never suppressed.

CCC V2 pool growth and syscall-bound wait-for tickets (2026-09-24):
the scheduler starts 2 workers and grows on backlog only when the
episode is not "run-to-park". A `@parallel wait` turnstile parks every
ticket (enter depth, `@stage` handshake), so project walk / search lanes
doing open / read stayed on 2 threads (1.7 CPUs busy on 4 cores).
`rtx_par_pool_up()` (`cc_parallel_noblock_prepare`) fills the pool
before the walk / search kick. The prepare fills once per process;
sysmon's slack settle (`sched_v2_settle_pool`) ignores the noblock pin,
but it only runs while a grow request is pending, which a full pool
never raises.

CCC V2 sysmon ticks forever (2026-09-27): the first `@parallel` in a
process (an image probe for a Markdown file with pictures, even with
pictures off; browse; find; a workbook read job) starts
`sched_v2_sysmon_main`, which waits on a 20 ms timeout for the rest of
the process: 50 wakeups a second at idle, the only thread that wakes
(measured 99-100 in 2 s, TUI and cctext-ui; the UI thread, GTK threads
and the parked workers stay at 0). Not a cctext loop, and nothing on
the cctext side can stop it (static, `pthread_once` init, no quiesce
API). Fixed upstream in concurrent-c #162 (in the pin since 7c19121):
sysmon waits with no timeout while nothing is queued / running /
growing / due and the pool is settled; a ready push, a worklet or a
park deadline pokes it. Every thread is now at 0 wakeups idle, and
`tests/tui_pty_test.py idle_threads`, `tests/ui_img_test.py` and
`tests/ui_blink_test.py` fail on the old 20 ms tick too
(`RTX_IDLE_STRICT=0` makes it a note, for an older runtime).

`@parallel wait` body locals' `@destroy` does not run when the body's
`@stage` wait fails (the lowering jumps to the construct's done label).
Browse lane scratch lives in `wk->wslot[]` and the arm frees it after
the join; a body-local scratch arena leaked 1 MiB per cancelled lane
(`@smoke_asan` LeakSanitizer).

RtxWs is ~400 KiB (eight `RtxBrowsePrev`, each a staging + shown
`RtxDoc`). Never declare one per test case in one function: at -O0 blocks
do not share stack slots and ASan keeps them apart (safe_smoke's main
needed 28 MiB). Smokes use `RtxWsHeap w_h = rtx_ws_heap() @destroy;
RtxWs *w = w_h.p;` (`tests/ws_heap.cch`). Reset big structs with
`memset`, not `*w = (RtxWs){0}` (a stack temporary at -O0). Check
frames with `--cc-flags -fstack-usage`; nothing in core/ is above
~70 KiB. A struct that moves by value (RtxDoc staging -> doc) must not
hold a pointer into itself: the piece tree's `arena = &arena_storage`
alias leaked the adopted preview's arena.

`@smoke_ubsan` (workbook / save / safe smokes) uses `scripts/ubsan.supp`:
ccc's `CCTask` stores pointer-aligned payloads at `_data` offset 4
(runtime/task.c `TASK_FIBER_V2`), reported on every `@parallel` spawn.
Runtime-only; cctext UB is never listed there.

`@smoke_tsan` on a host at load ~20 (4 cores): sysmon once declared
DEADLOCK DETECTED in replace_smoke with three fibers RUNNING and both
workers "idle" (exit 124), then TSan reported its own dump
(`sched_v2_debug_dump_state` reading `park_reason`). 0 of 4 reruns and
the next full list reproduced it; treat it as a runtime stall-detector
false positive, not a find race. Smoke timing windows (sindex racy
stamps) must not assume a 40 ms gap survives TSan on a loaded box.

`ui.cch` is owned by `ui.ccs` (`rtx_ui`). `ui_types.cch` is decls;
gutter / rail / blink bodies live in `workspace.ccs`. Tree chapters
(`piece_tree_rb.cch`, `piece_tree_lines.cch`, `piece_tree_priv.cch`)
are not TUs — include them only from `piece_tree.ccs`. Pair-join edit
paths go through `rtx_buf_pair_replace` (Rich pane); `RtxDoc_replace_join`
is markup policy on top of one `replace` (or one edit group), not a second
write. Multi-site commands go through `RtxDoc_replace_batch` — never a
loop of `replace` (that is n undo steps and n scan joins). Past
`RTX_GROUP_BULK_MIN` sites that call is a bulk swap and ONE hist record
(`RtxHistRec.bulk`, its own group): code that walks group members or
patches the last record's caret sees one record, not k. A bulk build
reads the tree and stages add bytes past `add_len`; it must not publish
(`commit_add`) or touch nodes before its commit joins find.

Two trees building at once (agent worktrees, a gate copy beside the
main checkout) share ccc's script cache `/tmp/cc-script-<uid>`: one
tree can pick up the other's compiled build script, which shows up as
intermittent "unknown type" or garbled generated-header errors that
vanish on a rerun. Fixed in concurrent-c 7c19121 (the pin): each
`.shcc` script stages under its own `$TMPDIR/cc-script-<uid>/<hash>/`.
With an older ccc, build one tree at a time or give each its own
`TMPDIR`.

# Compiler / toolchain notes

When a compile, lower, or link fails: **tell the user** (error text, TU).
Log the issue here if it is not a one-off typo.

## Dest-live `@parallel` captures

Pointer names copy the pointer. Other locals are by reference and must
outlive `.wait()`. Do not assign a dest-live worker arm to a stack `int`
(`noop = arm()`): that is still `&noop` after kick returns.

Browse, find, and island plants: `@serial { noop = … }` on the caller;
worker is an expression (`rtx_find_run` / `rtx_isle_run` /
`rtx_*_listing_arm`), not `noop = …`.

The handle owner and the worker argument must be different names.
`d->find.h = @parallel { rtx_find_run(d); }` lowers to `__e->d = &d`
(that slot is dead when the thunk runs). Copy into another pointer
first (`RtxDoc *scan = d`), the way browse stores the handle on `wk->h`
and passes `br`.

Destroy / drop: resume if paused, then `h.invalidate()`. `.wait()` does
not unpause and does not cancel adopted children.

## Long string literal in a local initializer is truncated

`const char *s = "…" "…" …;` *inside a function* silently truncates the
concatenated literal at 511 source bytes (lowered C ends mid-token, cc
then reports `missing terminating '"'` or a short string at run time).
File-scope initializers, a separate `s = "…" "…";` statement, and
literals in call arguments lower intact. Adjacent literals are not
concatenated in a call (`MK_DIR "/file"` → `expected ')' … found a
string literal`); build the path with snprintf. >2047 bytes is a proper
`token spelling exceeds 2047-byte AST text field` error. Put long
literals at file scope (see `mk_grammar` in tests/tm_grammar_smoke.ccs).

## `shadow_lower` stall (not reproduced)

2026‑09‑05: a `tests/tm_grammar_smoke.ccs` build sat in `shadow_lower` for
3+ min with no output while the IDE's `cc-lsp` was lowering the same file
and another project's test suite was compiling. Killing both and rerunning
the identical source passed in 3 s; a minimal repro of the new code (UFCS
call under `!` / `&&` with `&lo, &hi` args) lowers fine. Treat a silent
multi‑minute lower as contention first: check `pgrep -fl shadow_lower`.

## Clean lowerer `--root` is the compiler install (2026-09-18)

`ccc 0.4.0-415` (`--lowerer=clean`) invokes `cclower_cc --root ~/.local`.
A quoted header that resolves outside that prefix and outside the
including file's directory is rejected (`cannot place its lowered form`),
so `frontend/` → `../core/` includes fail. `scripts/cclower_root.py`
rewrites `--root` to the directory that contains `build.cc`. `make.shcc`
sets `CC_CLEAN_TOOL` to that wrapper.

A type tag may be introduced in only one file. `struct RtxNode;` in
`piece_tree.cch` plus the body in `piece_tree_priv.cch` is still two
definitions (`cannot extract piece_tree_rb.cch`). The node type lives
in `piece_tree.cch` only.

`!>;` / `@err` need an `@errhandler` in scope. An `@errhandler` that
unwraps nothing is an error. UFCS inside a function-pointer call
(`m->measure(ctx, bytes.sub(...), d->style_at(...))`) is not rewritten;
bind the slice and style first.

## Field UFCS on a Vec member

`d->runs.truncate(n)` / `L->rows.clear()` — method is on the Vec field,
including `@typehooks` owners (`RtxDoc`, `RtxLayout`). No `Vec::[T] *`
peel bind.

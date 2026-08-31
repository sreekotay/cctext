# Compiler / toolchain notes

When a compile, lower, or link fails: **tell the user** (error text, TU, what
looks stale vs real). Log the issue here if it is not a one-off typo.

## Stale lowered C (ccc incremental)

ccc writes C under `out/c/<hash>/…` and often **does not re-lower** a `.ccs`
when only an included `.cch` changed. clang then compiles **old** lowered
bodies against **new** extracted headers.

Symptom: `too many arguments` / wrong type, blamed on a `.cch` line that does
not match current source. Example: `ui.cch` already called `d->find_step()`,
cached `cctext_grid_draw` C still had `RtxDoc_find_step(d, RTX_FIND_BUDGET)`.
Also: arena UFCS re-lower — source has `d->session.alloc_slice_bytes(n)` /
`&d->session`, but stale cctext TUs still emit `(d->session)->a` which fails
once `cc_arena_*` macros wrap with `CC__ARENA_HOST` (`CCArena` is not a
pointer). Hit on `rtx_cctext_draw` / `_input` / `_grid_draw` after a ccc
arena-handle bump.

Fix: delete the stale `out/c/…/<tu>/*.c` (or that hash tree) and rebuild.
Do not “fix” the `.cch` to match the cached call.

## Dest-live `@parallel` captures

Pointer names copy the pointer. Other locals are by reference and must
outlive `.wait()`. Do not assign a dest-live worker arm to a stack `int`
(`noop = arm()`): that is still `&noop` after kick returns.

Browse, find, and island plants: `@serial { noop = … }` on the caller;
worker is an expression (`rtx_find_run` / `rtx_isle_run` /
`rtx_*_listing_arm`), not `noop = …`.

`!>` inside `document.cch` (early, before the TU’s result specs) failed
clang: `unknown type name 'CCResult_int_RtxIndexErr'` in `__cc_uw_is_err`.
Destroy / invalidate join with `cc__parallel_cancel_tree` + `cc_parallel_join`,
not UFCS `h.wait() !>`.

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

Destroy / invalidate join with `cc__parallel_cancel_tree` + `cc_parallel_join`,
not UFCS `h.wait() !>`.

## Field UFCS on a Vec member

`d->runs.truncate(n)` / `L->rows.clear()` — method is on the Vec field,
including `@typehooks` owners (`RtxDoc`, `RtxLayout`). No `Vec::[T] *`
peel bind.

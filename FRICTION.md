# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
raytext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

## Wrong-`E` unwrap diagnostics land in the emit product

`RtxIndexErr` is not a face of `CCError`. `@errhandler` picks the exact type.
The refusal is correct. The message is not.

A function with `@errhandler(RtxIndexErr)` that also writes `!>(e)` on a
`!>(CCError)` call (hit / save / `scratch_span`) fails as a C type error on
the lowered temp:

```text
initializing 'RtxIndexErr' with an expression of incompatible type 'CCError'
    RtxIndexErr e = (__r).u.error;
```

No source `!>(e)`, no "this call is `T !>(CCError)`, in-scope handler is
`RtxIndexErr`". `void !>(RtxIndexErr)` is missing from the unwrap `_Generic`,
so the default pretends the error is `CCError` (`NULL returned from box_begin`).
`(void)(t.line_start(n) !>)` is the one that *is* clear: unlowered `!>` in a
cast, refuse host C.

Want the diagnostic on the call: this Result's `E` vs the handler that bound
`e`, at the `.ccs` line. Workaround today: both handlers on the frame, `!>`
only (no `!>(e)`), and assign `line_*` to a local instead of casting.

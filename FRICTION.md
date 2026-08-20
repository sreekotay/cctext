# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
cctext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

`frontend/gui.ccs` sits on the 8192 AST-node cap (one TU with the core).
Find/glob field keys there stay compact; the TUI has the full set.
Browse listing (`rtx_browse_refresh` / `>` walk) is a linked TU (`core/browse.ccs`), same cut as hex.

`make.shcc` cannot `@string(..., @scratch)` — shcc does not declare
`__cc_str_scratch`. Pass a named `cc_arena_stack` (or heap) instead.

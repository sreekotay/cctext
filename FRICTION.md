# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
raytext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

- A non-static `char[:] !>(E)` in the tree TU drops family UFCS for the
  rest of that unit (`t->read_at` / `RtxPieceTree_read_at` “not declared”).
  `scratch_span` stays a header inline over `span` + `read_at`.

# Language friction (out-of-tree)

Nested local `.cch` includes are not lowered (nested file keeps `#!ccc` and
Result sugar). Do not `#include "other.cch"` from a `.cch` that is itself
included. Page store lives inside `piece_tree.cch` for that reason.

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

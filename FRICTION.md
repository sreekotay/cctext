# Language friction (out-of-tree)

Gaps found using Concurrent-C as a PATH toolchain, not a checkout-relative specimen.

- **Checkout `ccc` writes `out/` into the compiler repo** unless `--out-dir` / `--bin-dir` are absolute. `make` passes `$(CURDIR)/…`.
- **Line-1 `#!ccc` headers** copy the TU into a cache dir; quoted `#include` of project `.cch` files then miss the source tree. Sources use suffix kind; no `version=` pin.
- **UFCS does not see methods declared only in a spliced `.cch`.** Mitigated by `Type_method` names (`RtxPieceTree_len` → `t.len()`). If a call site fails to resolve, the free form is the same function.
- **Emit can hoist a struct ahead of a type it points at** (result-payload / field rewrite to `RtxNode*`). Forward `typedef struct` tags and `struct RtxNode *` fields.
- **No mmap helper** in stdlib. POSIX `mmap` + `cc_slice_from_buffer` (untracked).
- **C FFI is `@blocking`.** `cctext` / Raylib loops are blocking mains.

Closed in 0.3.3-139: bare CamelCase types from a passthrough `#include <…>` are seeded and emitted; host `cc` typechecks. `gui.ccs` includes `<raylib.h>` directly.

# Language friction (out-of-tree)

Gaps using Concurrent-C as a PATH toolchain. Present tense only.

- **Checkout `ccc` writes `out/` into the compiler repo** unless `--out-dir` / `--bin-dir` are absolute. `./make.shcc` uses `cc_script_ccc`, which prepends `<root>/out` and `<root>/bin`. The Makefile passes the same paths explicitly.
- **`make.shcc` needs scriptlib runners** (`cc_script_sh`, `cc_script_ccc`, `cc_script_sh_read`) in `<ccc/script/sh.cch>`. A `ccc` without those headers will not build the task file.
- **Line-1 `#!ccc` headers** copy the TU into a cache dir; quoted `#include` of project `.cch` files then miss the source tree. Sources use suffix kind; no `version=` pin.
- **Emit can hoist a struct ahead of a type it points at** (result-payload / field rewrite to `RtxNode*`). Forward `typedef struct` tags and `struct RtxNode *` fields.
- **`@scratch` is only the arena operand of `@string`.** Bind the product (`CCString line = @string(\`…\`, @scratch)`) before `return` or `cc_script_sh_read` — `return f(@string(…))` breaks `@destroy` return-rewrite, and a call-local `@string` is reclaimed after the consuming call. Do not `scratch.destroy()`.
- **`!>` into a pointer or field can fail to lower.** Unwrap into a local, then assign.
- **`cc_arena_stack` size is a compile-time constant** (`RTX_FRAME_SCRATCH`).
- **No mmap helper** in stdlib. POSIX `mmap` + `cc_slice_from_buffer` (untracked).

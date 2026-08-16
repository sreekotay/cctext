# Language friction (out-of-tree)

Gaps using Concurrent-C as a PATH toolchain. Present tense only.

- **`@scratch` is only the arena operand of `@string`.** Bind the product (`CCString line = @string(\`…\`, @scratch)`) before `return` or `cc_script_sh_read` — `return f(@string(…))` breaks `@destroy` return-rewrite, and a call-local `@string` is reclaimed after the consuming call. Do not `scratch.destroy()`.

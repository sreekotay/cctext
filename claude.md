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

## Product in an array bound of a header struct (2026-09-22)

`RtxMdCell lines[RTX_MD_TABLE_COLS * RTX_MD_WRAP_MAX];` in `layout.cch`
fails the clean lowerer's header extraction: `cannot extract layout.cch:
RTX_MD_TABLE_COLS* is not a type` (it reads `A * B` as a pointer type).
The target build then stops but a multi-target `ccc build` still exits
without an obvious error in a filtered log — check the binary's mtime.
Use a literal macro (`RTX_MD_REC_LINES 512`) and `_Static_assert` the
product in the `.ccs`.

## Tree chapters lower as their own module (2026-09-23)

`piece_tree_lines.cch` is spliced into the tree TU, but the clean lowerer
lowers it as a module first: `!>` on a `static` function of
`piece_tree.ccs` fails with `no visible declaration, so the producer could
not be typed as a Result`. Make it non-`static` and declare it in
`piece_tree_priv.cch`.

A `.ccs` cannot `#include` another `.ccs` (`is a source unit; a quoted
include names a face`). To build the tree TU with different `#ifndef`
knobs, pass `--cc-flags '-DRTX_...=...'` with its own `--out-dir` /
`--bin-dir` (`run_named_lix` in make.shcc).

## A face may not include a bare `.h` (2026-09-23)

The clean lowerer places each `.cch` under `out*/.cc-build/clean/` and
compiles it as its own module with no repo `-I`. A plain `.h` it
includes is not placed there, so `#include "ui_cmd.h"` / `<core/ui_cmd.h>`
from `ui_types.cch` failed every fresh build (`No such file or
directory`; a warm `out/` hid it). Shared C-only headers a face needs
are faces too (`core/ui_cmd.cch`); plain C (`gui_plat.h` for
`ui_plat.c`) includes the `.cch` by path.

`scripts/cclower_root.py` runs the `cclower_cc` beside `ccc` (`$CCC` or
PATH, symlinks resolved), then PATH, then `~/.local/bin`
(`CCLOWER_CC` overrides) — Docker installs to `/opt/ccc`.

## Sanitizer runs (2026-09-23)

`./make.shcc @smoke_tsan` adds `scripts/tsan.supp` to `TSAN_OPTIONS`
(runtime-internal reports only). Ubuntu 24.04 needs
`sysctl vm.mmap_rnd_bits=28` for TSan; Docker needs
`--security-opt seccomp=unconfined`. Pass `fast_unwind_on_fatal=1` in
`ASAN_OPTIONS`: the default slow unwinder dies on a fiber stack
(`nested bug in the same thread`). The turnstile cond-wake runtime bug
(FRICTION.md) fails `@smoke_asan` / flakes `@smoke_tsan` until
concurrent-c fixes it.

## 64 CC_TARGETs per build file (2026-09-23)

`ccc 0.4.0` stops with `cc: too many CC_TARGET entries or sources in
build.cc` once a build file declares a 65th `CC_TARGET`. Nothing else
fails, but adding a test to build.cc breaks every build. Tests past the
cap live in `build_tests.cc`, which keeps a copy of the core library
targets. `make.shcc`'s `build_file_of(name)` routes `run_named` and the
ASan / TSan runs there. Keep the two library blocks equal.

A target may list several sources (`CC_TARGET rtx_md_table obj
core/md_table.ccs core/md_block.ccs`); that adds no `CC_TARGET` entry,
so a new library TU can join an existing target instead of taking a
slot (65 sources over 64 targets builds). `ccc build --build-file F a b`
builds only `a` (2026-09-23): build targets one call each.

## `memrchr` is not on macOS (2026-09-25)

A bare `void *memrchr(...);` in `core/rx.ccs` links on glibc and fails
everywhere else (`Undefined symbols: _memrchr`, first seen linking
`utf8_cluster_smoke`). Provide a static fallback unless `__GLIBC__`.
`memmem` is fine: macOS has it.

## ccc 0.4.0-418 truncates long link lines (2026-09-25)

Links of `cctext`, `cctext-ui`, `layout_measure_smoke` failed with
`ld: file cannot be mmap()ed, errno=22 path=/Users` or `clang: error:
no such file or directory: '/User'`: the host-cc command is cut at about
4 KB. Fixed in concurrent-c `f24fff67` (growable `CCCmd`); the CI
`CCC_PIN` (`0a633969`, 0.4.0-419) predates it. A side install of a
newer commit works: `PREFIX=$HOME/.ccc-e59f ./cc-install.sh
--no-add-to-path --no-editor-tools`, then build with that `bin/` first
on PATH.

The IDE's `cc-lsp` rewrites `out/.cc-build/clean/*.h` while a build
reads them. A one-off `unknown type name` / `undeclared identifier` for
a type the face plainly declares (`RtxMdbLine`, `RtxUtf8Cl`) is that
race; rerun before debugging.

## Smoke fixtures on macOS (2026-09-25)

APFS rejects file names that are not valid UTF-8 (`EILSEQ`), so
`term_safe_smoke` skips that fixture there. macOS `TMPDIR` is under
`/var` → `/private/var`; compare against `realpath` output
(`proj_smoke` canonicalizes its root).

## Static functions in a face's file-scope initializer (2026-09-26)

A file-scope `static const RtxCmdDef rows[] = { …, my_static_fn }` in a
`.cch` face whose `my_static_fn` is `static` in the same face fails the
host compile: `'my_static_fn' undeclared here (not in a function)` — the
lowered header keeps the table but not the static bodies. Make the
functions non-static (the face's owner `.ccs` links them) and fill the
row inside a function (`rtx_ui_wb_register`).

## No libm in the link (2026-09-26)

Targets link without `-lm`: `floor` / `ceil` / `round` / `fmod` are
undefined references at link time (`fabs`, `isnan`, `isfinite` are
builtins and fine). `core/wb.ccs` carries its own `wb_floor` & co.

`RtxDoc_from_buffer` keeps the slice as the original buffer (no copy)
and refuses an untracked one (`cc_slice_from_buffer` → `untracked
buffer`): tests clone into an arena that outlives the doc
(`src.clone_into(a)`, as edit_group_smoke does).

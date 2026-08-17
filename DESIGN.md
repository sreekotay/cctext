# Design

One document core, two frontends (`cctext`, `raytext`). Memory is owned or it is a view. Lifetime is a field, not a protocol. Ownership is handled at the call site.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change. A constructor does not tear down a live object. Reopen is two lines:

```ccs
d.destroy();
d.from_path(path) !>;
```

or a second local (`RtxDoc d2 = {0} @destroy`). `memset` is neither.

Hooks and faces live next to the type they name (`piece_tree.cch`, `document.cch`, …). A Layout parameter cannot `insert` — that is local in the signature, not a comment. Frame copies take a scratch arena the function owns; analysis copies take `d.analysis`. Hist and path stay on `session` across reparse.

There is no inflight counter and no drain-to-zero. A path that gives up says so at the position that caused it.

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Document | piece-tree arena + mmap | `RtxDoc.destroy()` |
| Session | `d.session` | close (path, undo) |
| Analysis | `d.analysis` | `analysis.reset()` on reparse (sections, runs) |
| Find | `d.find.store` | new query resets; edit invalidates (offsets only) |
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs, clipboard) |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

`RtxDoc d = {0} @destroy` (and the same for tree, layout, buf, workspace). No inflight counter. A buf slot destroys layout, then doc.

## Surfaces

Fallible APIs are Results (`T !>(CCError)`). Value returns are only pure queries of already-valid state (`len`, `line_*`, `has_sel`, `dirty`, …). OOM, IO, and a short mid-document read are errors — never a short slice or a zero that looks like success on a commit path.

One document read surface: `size_t !>(CCError) read_at(off, dest, n)`. Success returns `got = min(n, len - off)` (EOF clamp is success). A hole inside that range is an error. Callers that need every byte of `n` require `off + n <= len` (or check `got == n`). There is no second “unchecked” path.

If an API returns owned bytes, the destination arena is the **last** parameter (Concurrent-C convention: receiver first, arena last). That arena *is* the product’s lifetime. Call-local `@scratch` / frame stack stays inside the callee and is not returned. Views (`span`) do not take an arena.

A Result failure is **unchanged** or **`broken`**. `broken` means the tree may be inconsistent — set only after a mutating step that could not be rolled back. Pre-mutation faults (failed hold alloc, short read before erase) leave the object intact and do not set `broken`.

Hist (and any other commit record) stores everything the next edit needs, or undo/redo clears what it does not restore. Derived mode that affects the next op (`sel_box`, box columns, …) is either in the record or fail-closed to stream selection.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — `cc_file_map`, stamped with the tree arena. The `CCMappedFile` lives on the tree until `destroy()`.
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`). That is a payload.
- `scratch_span` / `analysis_span` — view if contiguous, else a copy on the named arena. `char[:] !>(CCError)`: empty is `n == 0` / past end; OOM and short `read_at` are errors.

## Safety

Constructors assume dead. `from_path` / `from_buffer` / `empty` / `open_files` error if the object already owns an arena or a mapping. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success:

- Seed of a non-empty original must produce a root node, or the constructor fails.
- Scan / highlight / reparse / `ensure_hl` are `void !>(CCError)`. They do not plant markup or set `hl_done` after a missing span. A failed scan resets analysis. A failed highlight strips hl-only runs and clears every `hl_done`.
- `RtxWs_copy` is `int !>(CCError)`: `0` = no selection, `1` = copied, OOM is an error.
- A short `read_at` mid-document is a fault, not EOF. `line_off_ok` is set only after the scan reaches `len`. An edit truncates the stride index at the first slot at/after the edit and rebuilds only that suffix — not a full-file rescan when the index was already warm.

Commit only after the new value exists, in every direction: the right node before shrinking a piece; hist after `tree.replace`; clip after a successful cut replace; derived flags after the highlight / line-index pass; path+`saved_head` after a prepared rename. Unsaved quit opens a Save / Don't save / Cancel prompt. `tree.replace` rolls the deleted span back if insert fails; rollback failure sets `d.broken` and further edits refuse. Clipboard allocs into a local, then assigns; OOM keeps the old clip. Empty source is a real clear. A path that gives up is either unchanged or `broken` — never a hole that looks retryable.

Host close is a one-shot offer into that prompt, not loop control. Cancel dismisses this close request; it does not promise a second chrome-X if the host latches `shouldClose`.

## Faces

`@typehooks` / `@typeview` sit next to the types.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — UFCS that misses retries on the embed. A projection, not a lock.
- `RtxDocLayout` — named allow-list: measure may `len`, `line_*`, `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl`. It cannot `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

`L.wrap` is a layout policy, not a section kind. Off: one visual row per physical line. On: last whitespace if the next token fits after it, else a hard break at `max_width`. A token wider than the window still occupies a row. Scroll is `top` (physical line) plus `top_wrap` (visual rows skipped on that line). Up/down walk visual rows and keep a goal column.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for mmap / insert / `write_fd`.

## Edits

User changes are `replace` on the history stack (type / backspace / delete). Save streams pieces (`write_fd`), fsyncs, renames. Dirty is `hist.head != saved_head`. Offsets are bytes. Stream selection is `[sel_anchor, caret)`. Up/down keep a goal column (`pref.col` / `pref.x`) so a short line does not forget the place; End sets `pref.eol` and sticks to each line end. Alt-arrows / Alt-drag is a column box (`sel_box`): each line contributes `[box_acol, box_ccol)` (virtual; short lines clamp). Copy joins those slices with newlines. Type/backspace apply the same column on every line as one replace.

Find stores offsets on `d.find.store`. A frame steps at most 256 KiB so a giant file does not stall. Context is a line window computed when drawing the visible hits. `f` / Ctrl-F opens the panel; up/down moves among hits.

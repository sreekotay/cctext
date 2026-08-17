# Design

One document core, two frontends (`cctext`, `raytext`). Memory is owned or it is a view. Lifetime is a field, not a protocol. Ownership is handled at the call site.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change. A constructor does not tear down a live object. Reopen is two lines:

```ccs
d.destroy();
d.from_path(path) !>;
```

or a second local (`RtxDoc d2 = {0} @destroy`). `memset` is neither.

Hooks and faces live next to the type they name (`page_store.cch`, `piece_tree.cch`, `document.cch`, …). Leaf modules that are real linked TUs: `page_store.ccs`, `hex.ccs` (decls in the matching `.cch`). The piece tree is still split for size with textual includes (`piece_tree_rb.cch`, `piece_tree_lines.cch`) into one lowered unit with the caller. A Layout parameter cannot `insert` — that is local in the signature, not a comment. Frame copies take a scratch arena the function owns; analysis copies take `d.analysis`. Hist and path stay on `session` across reparse.

There is no inflight counter and no drain-to-zero. A path that gives up says so at the position that caused it.

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Document | piece-tree arena + page-store arena (fds + LRU page pool) | `RtxDoc.destroy()` |
| Session | `d.session` | close (path, undo) |
| Analysis | `d.analysis` | `analysis.reset()` on reparse (sections, runs) |
| Find | `d.find.store` | new query resets; edit invalidates (offsets only) |
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs, clipboard) |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

`RtxDoc d = {0} @destroy` (and the same for tree, layout, buf, workspace). No inflight counter. A buf slot destroys layout, then doc.

## Surfaces

Fallible APIs are Results (`T !>(CCError)`). Value returns are only pure queries of already-valid state (`len`, `has_sel`, `dirty`, …). OOM, IO, and a short mid-document read are errors — never a short slice or a zero that looks like success on a commit path.

`line_start` / `line_of` are `size_t !>(RtxIndexErr)`. `RtxIndexErr` is not a face of `CCError`. `@errhandler` matches exact `E`. Helpers that call `line_*` are `T !>(RtxIndexErr)` and pipe. A `!>(CCError)` surface that also does index work translates once (`CC_ERR_IO`). Paint / key (and test `main`) abandon the frame. A scan fault sets `line_index_fault` (query: `index_fault()`) and clears `line_off_ok` (status `Lcur!`).

One document read surface: `size_t !>(CCError) read_at(off, dest, n)`. Success returns `got = min(n, len - off)` (EOF clamp is success). A hole inside that range is an error. Callers that need every byte of `n` require `off + n <= len` (or check `got == n`).

If an API returns owned bytes, the destination arena is the **last** parameter (Concurrent-C convention: receiver first, arena last). That arena *is* the product’s lifetime. Call-local `@scratch` / frame stack stays inside the callee and is not returned. Views (`span`) do not take an arena.

A Result failure is **unchanged** or **`broken`**. `broken` means the tree may be inconsistent — set only after a mutating step that could not be rolled back. Pre-mutation faults (failed hold alloc, short read before erase) leave the object intact and do not set `broken`.

Hist (and any other commit record) stores everything the next edit needs, or undo/redo clears what it does not restore. Derived mode that affects the next op (`sel_box`, box columns, …) is in the hist record (before/after); undo/redo restores it.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — opens the path into the tree’s page store (fds + page cache) until `destroy()`. There is no document-wide `char[:]` over the file; bytes are reached only through `read_at` (or `scratch_span` / `analysis_span` copies onto a named arena). Open does not scan the body; newline weights and the stride index grow **progressively** on `line_start` / `line_of`. Markup/`===` scan is skipped above `RTX_MARKUP_SCAN_MAX` (single section; path may still mark code).
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`), or store-backed bytes with no stable contiguous view. That is a payload; callers use `scratch_span` / `read_at`.
- `scratch_span` / `analysis_span` — view if contiguous, else a copy on the named arena. `char[:] !>(CCError)`: empty is `n == 0` / past end; OOM and short `read_at` are errors.

## Safety

Constructors assume dead. `from_path` / `from_buffer` / `empty` / `open_files` error if the object already owns an arena, a page store, or a borrowed original binding. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success:

- Seed of a non-empty original must produce a root node, or the constructor fails.
- Scan / highlight / reparse / `ensure_hl` are `void !>(CCError)`. They do not plant markup or set `hl_done` after a missing span. A failed scan resets analysis. A failed highlight strips hl-only runs and clears every `hl_done`. For code sections larger than `RTX_HL_FULL_MAX`, `ensure_hl` lexes only the visible window and leaves `hl_done` clear so scroll re-lexes — it never copies the whole body for analysis.
- `RtxWs_copy` is `int !>(CCError)`: `0` = no selection, `1` = copied, OOM is an error.
- A short `read_at` mid-document is a fault, not EOF. `line_count` is soft until EOF (`known + 1MiB` scroll budget only). `line_known` / `Lcur+` are what the index has seen; `lf_ready` makes `Lcur/total` exact. `line_off_ok` is set when the scan reaches `len`. Above `RTX_LINE_SOFT_MIN`, an edit through an offset the index has not reached still scans to that offset, then clears `lf_ready` and does not rebuild a suffix. Small files finish the index eagerly. A tip insert at EOF extends the add-piece without a rescan. Long scans and saves pulse `t.busy_bind` / `w.busy_bind`; headless leaves it unbound.

Commit only after the new value exists, in every direction: the right node before shrinking a piece; hist after `tree.replace`; clip after a successful cut replace; derived flags after the highlight / line-index pass; path+`saved_head` after a prepared rename. Unsaved quit opens a Save / Don't save / Cancel prompt. `tree.replace` rolls the deleted span back if insert fails; rollback failure sets `d.broken` and further edits refuse. Clipboard allocs into a local, then assigns; OOM keeps the old clip. Empty source is a real clear. A path that gives up is either unchanged or `broken` — never a hole that looks retryable.

Host close is a one-shot offer into that prompt, not loop control. Cancel dismisses this close request; it does not promise a second chrome-X if the host latches `shouldClose`.

## Faces

`@typehooks` / `@typeview` sit next to the types they name. A `@typeview` is the application’s allow-list. An `as:` embed retries UFCS on the inner type when the face grants the name.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — miss on the outer retries on the embed.
- `RtxDocLayout` — named allow-list: measure may `len`, `line_*`, `line_guess`, `index_covers`, `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl`. It cannot `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

`L.view` is the layout policy (`RTX_VIEW_DEFAULT` / `WRAP` / `HEX`); `L.wrap` is 1 iff wrap. Default: one visual row per physical line. Wrap: last whitespace if the next token fits after it, else a hard break at `max_width`. Hex: byte rows of 16 with offset | hex | ASCII columns (synced selection); scroll via `top_byte`. A token wider than the window still occupies a row in wrap. Text scroll is `top` (physical line) plus `top_wrap`. Up/down walk visual rows (or ±16 bytes in hex) and keep a goal column in text modes. Ctrl-L cycles the three views. Ctrl-U toggles follow lock: off, the pane does not chase the caret (wheel / rail stay); find and jump still `reveal_off` in the active view (hex `%` keeps the byte). Both frontends blink the caret (400 ms) and skip a full relayout while idle. Both draw a byte-rail scrollbar (`pos` / `len`, not soft `line_count`): Raylib pixels, TTY a reserved column. The rail is hidden when the pane already shows the whole file. Click/drag maps to a byte and snaps like jump `N%`. A seek past the scan frontier fills a local byte window (`fill_off` / `line_guess`) — it does not `line_of` the prefix. The rail is marked while `!lf_ready`.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for insert / `write_fd` / page-store internals.

## Edits

A same-file split is two cameras on one document. After a mutation, `RtxWs_after_edit` reparses once and drops every matching camera's vis-row epoch — `ensure_view`'s width/top cache is not an edit stamp, so a mid-line delete would otherwise leave the unfocused pane painting stale byte ranges.

User changes are `replace` on the history stack (type / backspace / delete). Large deletes (`> RTX_HIST_INLINE_MAX`) omit inline bytes — undo of those records fails closed; redo still works. Coalesced typing grows hist byte slots with capacity doubling. Save streams pieces (`write_fd`), preserves existing mode when overwriting, fsyncs the file (and best-effort the parent directory), then renames. Dirty is `hist.head != saved_head`. Offsets are bytes. Stream selection is `[sel_anchor, caret)`. Up/down keep a goal column (`pref.col` / `pref.x`) so a short line does not forget the place; End sets `pref.eol` and sticks to each line end. Alt-arrows / Alt-drag is a column box (`sel_box`): each line contributes `[box_acol, box_ccol)` (virtual; short lines clamp). Copy joins those slices with newlines. Type/backspace apply the same column on every line as one replace.

Find stores offsets on `d.find.store`. A frame steps at most 256 KiB so a giant file does not stall. Context is a line window computed when drawing the visible hits. `f` / Ctrl-F opens the panel; up/down moves among hits.

`o` / Ctrl-O opens a file in the focused view. The TTY uses a per-pane browser (directory listing + glob). The GUI Open… command is the system file dialog and still calls `RtxWs_open_in_pane` (live; no restart). Ctrl-Shift-O keeps the in-app browser as a reference. Enter on a directory walks; Enter on a file points that pane at the path. A same-file split keeps the other camera on the old buf. The process cwd does not change.

## Encoding

Offsets, caret, selection, and the piece tree are bytes (UTF-8 code units treated as opaque). A later grapheme / display-width layer sits on top of that; `read_at` stays bytes.

# Design

One document core, two frontends (`cctext`, `cctext-ray`). Memory is owned or it is a view. Lifetime is a field, not a protocol. The document type is as wide as the domain — a face is reach at the call site, not a smaller struct. Epochs say what dies when; faces say what this call may do. A TU’s write is the function that accepts only legal values for that unit; that is what the header exports. Ownership is handled at the call site.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change. A constructor does not tear down a live object. Reopen is two lines:

```ccs
d.destroy();
d.from_path(path) !>;
```

or a second local (`RtxDoc d2 = {0} @destroy`). `memset` is neither.

Hooks and faces live next to the type they name. A file cut is not an API — chapters of one TU stay chapters (`piece_tree_rb` / `piece_tree_lines` are not their own products). Do not extract the next linked TU (document, layout, workspace) because the tree did; wait until compile time hurts. The header exports the legal write (`replace`), not `insert` / `erase` because a rope usually does. Do not split `RtxDoc` into history/selection types because it is wide. A label on a shared record is an enum (closed dispatch, no `default:`); a value that is one of several payloads is a `@variant` — do not variant-shape every classifier. A Layout parameter cannot `insert` — that is local in the signature, not a comment. Frame copies take a scratch arena the function owns; analysis copies take `d.analysis`. Hist and path stay on `session` across reparse.

There is no inflight counter and no drain-to-zero. A path that gives up says so at the position that caused it.

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Document | piece-tree arena + page-store arena (fds + LRU page pool) | `RtxDoc.destroy()` |
| Session | `d.session` | close (path, undo) |
| Analysis | `d.analysis` | `analysis.reset()` on reparse |
| Find | `d.find.store` | new query resets; edit invalidates (offsets only) |
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs, clipboard) |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

`RtxDoc d = {0} @destroy` (and the same for tree, layout, buf, workspace). A buf slot destroys layout, then doc.

## Surfaces

Fallible APIs are Results (`T !>(CCError)`). Value returns are only pure queries of already-valid state (`len`, `has_sel`, `dirty`, …). OOM, IO, and a short mid-document read are errors — never a short slice or a zero that looks like success on a commit path.

`line_start` / `line_of` are `size_t !>(RtxIndexErr)`. `RtxIndexErr` is not a face of `CCError` — do not `@typeview { as: base; }` so save/edit handlers cannot swallow an index fault. Helpers that call `line_*` are `T !>(RtxIndexErr)` and pipe. A `!>(CCError)` surface that also does index work translates once.

One document read surface: `size_t !>(CCError) read_at(off, dest, n)`. Success returns `got = min(n, len - off)` (EOF clamp is success). A hole inside that range is an error. Callers that need every byte of `n` require `off + n <= len` (or check `got == n`).

If an API returns owned bytes, the destination arena is the **last** parameter (receiver first, arena last). That arena *is* the product’s lifetime. Call-local `@scratch` / frame stack stays inside the callee and is not returned. Views (`span`) do not take an arena.

A Result failure is **unchanged** or **`broken`**. `broken` means the tree may be inconsistent — set only after a mutating step that could not be rolled back. The latch is a kind (`RTX_ERR_UNRESTORABLE`), not a message. Pre-mutation faults leave the object intact and do not set `broken`.

Hist stores everything the next edit needs, or undo/redo clears what it does not restore. Derived mode that affects the next op is in the hist record; undo/redo restores it.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — page store until `destroy()`. No document-wide `char[:]` over the file; bytes are `read_at` (or a named-arena copy). Open does not scan the body; the line index grows on `line_start` / `line_of`.
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`), or store-backed bytes with no stable view. That is a payload; callers use `scratch_span` / `read_at`.
- `scratch_span` / `analysis_span` — view if contiguous, else a copy on the named arena. Empty is `n == 0` / past end; OOM and short `read_at` are errors.

## Safety

Constructors assume dead. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success: a non-empty original must produce a root; scan / highlight / reparse do not plant markup or set `hl_done` after a missing span. Lex a window, not the body — do not pass `len` as a highlight bound. A TM-backed path is `CODE` on the first window; mixed markup stays `UNKNOWN` until a header or BOF — do not invent a path-default for a file with no grammar. Lex copies die with the frame, not an `analysis` bump. A short `read_at` mid-document is a fault, not EOF. `line_count` is soft until the index reaches EOF.

Commit only after the new value exists: hist after `tree.replace` (reserve coalesced bytes before, commit after); clip after a successful cut; path + `saved_head` after a prepared rename. Rollback failure is `RTX_ERR_UNRESTORABLE` and sets `d.broken` — further edits refuse. Clipboard allocs into a local, then assigns. Empty source is a real clear. A path that gives up is unchanged or `broken` — never a hole that looks retryable.

## Faces

`@typehooks` / `@typeview` sit next to the types they name. A `@typeview` is the application’s allow-list. An `as:` embed retries UFCS on the inner type when the face grants the name. A face fences callers that take the face, not every `RtxDoc *` in the same file. Field writes and `as:` embeds are not gated unless the parameter is the face. Add the next face when a new function would otherwise take `RtxDoc *` and only need a slice — not a suite of faces because the type is wide.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — miss on the outer retries on the embed.
- `RtxDocHighlight` — `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl(RtxHlWin)`. It cannot `len` / `line_*` / `insert` / `type` / `save`.
- `RtxDocLayout` — measure may `len`, `line_*`, `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl(RtxHlWin)`. It cannot `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for `write_fd` / page-store internals.

## Edits

A same-file split is two cameras on one document. After a mutation, reparse once and drop every matching camera’s vis-row epoch — a width/top cache is not an edit stamp.

The document write is `replace` (byte range in `[0, len]`). `insert` / `erase` on the doc call it. User changes are `replace` on the history stack. Dirty is `hist.head != saved_head`. Offsets, caret, and selection are bytes. Save streams pieces (`write_fd`).

## Encoding

Offsets, caret, selection, and the piece tree are bytes. Text views walk
**clusters** for motion, wrap, hit-test, backspace, and measure (a UTF-8 scalar
plus following combining marks / variation selectors). Hex views stay a byte
camera: left/right, backspace, delete, box, and home/end step one byte; the
ASCII dump is one cell per byte. Leaving hex with a collapsed caret snaps it
to a cluster start. `read_at` stays bytes. Invalid bytes are one-byte clusters
(U+FFFD, width 1). Full UAX #29 graphemes (ZWJ emoji, flags) are later.

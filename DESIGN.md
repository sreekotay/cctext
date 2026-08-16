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
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs, clipboard) |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

`RtxDoc d = {0} @destroy` (and the same for tree, layout, buf, workspace). No inflight counter. A buf slot destroys layout, then doc.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — mmap, stamped with the tree arena. Mapping is owned (`map_owned`).
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`). That is a payload.
- `scratch_span` / `analysis_span` — view if contiguous, else a copy on the named arena. `char[:] !>(CCError)`: empty is `n == 0` / past end; OOM and short `read_at` are errors.

## Safety

Constructors assume dead. `from_path` / `from_buffer` / `empty` / `open_files` error if the object already owns an arena or a mapping. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success:

- Seed of a non-empty original must produce a root node, or the constructor fails.
- Scan / highlight / reparse / `ensure_hl` are `void !>(CCError)`. They do not plant markup or set `hl_done` after a missing span. A failed scan resets analysis.
- `RtxWs_copy` is `int !>(CCError)`: `0` = no selection, `1` = copied, OOM is an error.

Commit only after the new value exists. Hist payload is allocated before erase/insert; redo is dropped only after apply succeeds. Insert-fail after erase is `CC_ERR_INTERNAL` (half-applied), not “unchanged.” Clipboard allocs into a local, then assigns; OOM keeps the old clip. Empty source is a real clear.

`len` / `line_*` / `has_sel` / `dirty` do not fail. They stay values.

## Faces

`@typehooks` / `@typeview` sit next to the types.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — UFCS that misses retries on the embed. A projection, not a lock.
- `RtxDocLayout` — named allow-list: measure may `len`, `line_*`, `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl`. It cannot `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for mmap / insert / `write_fd`.

## Edits

User changes are `replace` on the history stack (type / backspace / delete). Save streams pieces (`write_fd`), fsyncs, renames. Dirty is `hist.head != saved_head`. Offsets are bytes. Selection is `[sel_anchor, caret)`.

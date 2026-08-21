# Design

One document core, two frontends (`cctext`, `cctext-ray`). Memory is owned or it is a view. Lifetime is a field, not a protocol. The document type is as wide as the domain — a face is reach at the call site, not a smaller struct. Epochs say what dies when; faces say what this call may do. A TU’s write is the function that accepts only legal values for that unit; that is what the header exports. Ownership is handled at the call site.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change. A constructor does not tear down a live object. Reopen is two lines:

```ccs
d.destroy();
d.from_path(path) !>;
```

or a second local (`RtxDoc d2 = {0} @destroy`). Same for tree, layout, buf,
workspace. A buf slot destroys layout, then doc. `memset` is neither.

Hooks and faces live next to the type they name. A file cut is not an API — chapters of one TU stay chapters (`piece_tree_rb` / `piece_tree_lines` are not their own products). Do not extract the next linked TU (document, layout, workspace) because the tree did; wait until compile time hurts. The header exports the legal write (`replace`), not `insert` / `erase` because a rope usually does. Do not split `RtxDoc` into history/selection types because it is wide. A label on a shared record is an enum (closed dispatch, no `default:`); a value that is one of several payloads is a `@variant` — do not variant-shape every classifier. A Layout parameter cannot `insert` — that is local in the signature, not a comment. Frame copies take a scratch arena the function owns; analysis copies take `d.analysis`. Hist and path stay on `session` across reparse.

There is no inflight counter and no drain-to-zero. A path that gives up says so at the position that caused it.

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Document | piece-tree arena + page-store arena (fds + LRU page pool) | `RtxDoc.destroy()` |
| Session | `d.session` | close (path, undo) |
| Analysis | `d.analysis` | `analysis.reset()` on reparse |
| Find | `d.find.store` (query + offs) | new query resets; `RtxDoc.destroy()`; edit invalidates offsets |
| Find wave | call-local heap in `find_step` | end of the step (~4 MiB; not the find store) |
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs, clipboard) |
| Browse | `br.store` ents + `br.walk` jobs | kick resets; drop destroys |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

## Interactive

The UI thread does not wait on the line index. Open, paint, hit, select,
arrow, wheel, `%` jump, and a gap edit are bytes: `line_floor` / `line_next`
(local 8KiB), `fill_off`, window lex. Gutter is `+N` / `-L` until the
island meets the prefix.

Two cameras, two window writes — do not mix them.

| camera | origin | fill |
|---|---|---|
| seek | `seek_off` | `fill_off` |
| line | `top` | `fill` |

Unwrapped text also keeps `left_col` (window x, same units as the
layout width). End, a caret past the pane, wheel left/right, Shift-
wheel, a tilt wheel, wheel on the bottom bar, or a drag past the pane
edge moves it. Cursor/VS Code drop Shift+wheel `deltaX`; the TTY
recovers it from the session while focused.
Wrap and hex stay at 0. Grid is the same offset, not a second camera.
The bottom bar appears only when a vis row is wider than the pane.

`line_of(seek_off)` hands off to the line camera (`top` / `fill`).
`index_covers` alone does not — that wrote `line_guess` into `top`.
Unlabeled stays a seek.

| window | caret already visible | else |
|---|---|---|
| `reveal` (follow) | keep | scroll / `seek_set` |
| `land` | snap that line to top | `seek_set` |

Click, type, and `ensure_caret` follow. Jump `%` and find hits land.
`if (seek) land else reveal` is the bug: a seek is a camera, not
“index incomplete”. After handoff, wrap follow owns `top_wrap` again.

`line_of` is a covered lookup. Uncovered is an error — it does not scan.
`RtxDocLayout` does not grant it. Box select is two floors and columns,
not line numbers. `line_count` is soft until EOF. `line_start` may extend
the prefix; do not call it from a gap camera.

`g N%` is a byte snap plus island (backfill). `g L` is a prefix pump
(`want_line`), same host gate as find — not a guess. After open the
prefix also walks toward EOF on that gate (`prefix_step`) so gutters
and `L n/N` do not sit idle.

`lf_ready` the field is a latch. `lf_ready()` / `index_covers` are
false when the flag is stale (huge + `subtree_lf==0`). The flag
means node newline weights match the body (one unedited original at
EOF, or a small recount). It is not “the prefix reached a milestone.”
A split tree at EOF stays progressive: `scan_line` is exact, weights
are not. Setting the flag with `subtree_lf==0` made `line_count()==1`
and snapped the camera to line 0.

The prefix frontier is paid work. A covered edit patches it
(`note_insert` / `note_erase`). Truncate at the caret only when the
edit changes newlines (Enter may park the scan). A letter key that
parked `line_scan_off` at the caret rescanned the tail.

`line_count` is a scroll budget until weights are live. A window
write must not take `to = len` because that total said “last page.”

Host frame: mutations, then one window write. TUI read blocks, so
`pump → layout+paint → input`. GUI events are already polled, so
`input → pump → layout → paint`. Do not layout, then handle, then
layout again — a command that forgets a dirty bit paints the old camera.

## Scan

Backfill is a pump. The host yields (`rtx_ui_work_pump`). Do not extract a
shared type — bits stay local (`find.done`, `browse.scanning`, GUI AST).
The next walk copies this table.

| | start | one wave | live | resume | deny |
|---|---|---|---|---|---|
| find | `find_set` | `find_step` | `!done && !cancel` | `scan_off` | `RTX_FIND_SEQ` |
| jump | `want_kick` | `want_step` | `want_pumping` | `line_scan_off` | — |
| prefix | open / `!lf_ready` | `prefix_step` | `prefix_pumping` | `line_scan_off` | — |
| island | `isle_kick` | `isle_step` | `isle_pumping` | `isle_from` | — |
| browse | `rtx_browse_kick` | `rtx_browse_pump` | `scanning` | job queue | `RTX_BROWSE_SEQ` |

Kick plants the first paint and returns. One closed interval per step;
returning is the yield. The find wave is a heap arena with `@destroy`;
a Result return after `@parallel wait` runs that life (ccc 0.3.4-194).
Query copy and hit offsets stay on `d.find.store` and die with the
document. A
longer prefix query filters hits and keeps `scan_off`; a cap resumes
from the last accepted hit; a shorter or non-prefix query resets.
`find_apply` enters once; `find_pump` may enter again up to
`RTX_FIND_PUMP` while `RTX_FIND_PUMP_MS` remains, then lands.
The step returns staged hops; 0 is not progress. `cancel` is a written
bit, not a stdin poll. Chrome that owns the walk shows `scanning...` /
`capped`. Tests call `finish` (drain). Do not drain the first screen
before first paint. Tests that need a covered `line_of` call
`rtx_line_scan_to` (or a pump) first.

## Surfaces

Fallible APIs are Results (`T !>(CCError)`). Value returns are only pure queries of already-valid state (`len`, `has_sel`, `dirty`, …). OOM, IO, and a short mid-document read are errors — never a short slice or a zero that looks like success on a commit path.

`line_start` / `line_of` are `size_t !>(RtxIndexErr)`. `RtxIndexErr` is not a face of `CCError` — do not `@typeview { as: base; }` so save/edit handlers cannot swallow an index fault. Helpers that call `line_*` are `T !>(RtxIndexErr)` and pipe. A `!>(CCError)` surface that also does index work translates once.

One document read surface: `size_t !>(CCError) read_at(off, dest, n)`. Success returns `got = min(n, len - off)` (EOF clamp is success). A hole inside that range is an error. Callers that need every byte of `n` require `off + n <= len` (or check `got == n`).

If an API returns owned bytes, the destination arena is the **last** parameter (receiver first, arena last). That arena *is* the product’s lifetime. Call-local `@scratch` / frame stack stays inside the callee and is not returned. Views (`span`) do not take an arena.

A Result failure is **unchanged** or **`broken`**. `broken` means the tree may be inconsistent — set only after a mutating step that could not be rolled back. The latch is a kind (`RTX_ERR_UNRESTORABLE`), not a message. Pre-mutation faults leave the object intact and do not set `broken`.

Hist stores everything the next edit needs, or undo/redo clears what it does not restore. Derived mode that affects the next op is in the hist record; undo/redo restores it.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — page store until `destroy()`. No document-wide `char[:]` over the file; bytes are `read_at` (or a named-arena copy). Open does not scan the body.
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`), or store-backed bytes with no stable view. That is a payload; callers use `scratch_span` / `read_at`.
- `scratch_span` / `analysis_span` — view if contiguous, else a copy on the named arena. Empty is `n == 0` / past end; OOM and short `read_at` are errors.

## Safety

Constructors assume dead. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success: a non-empty original must produce a root; scan / highlight / reparse do not plant markup or set `hl_done` after a missing span. Lex a window, not the body — do not pass `len` as a highlight bound. The **root section is the path kind** (`CODE` if a grammar matches, else `PROSE`). `===` headers still split. Mixed markup stays `UNKNOWN` until a header or BOF — do not invent a path-default for a file with no grammar. `*` / `` ` `` refresh style runs; `=` or a large delete rescans sections. Lex copies die with the frame, not an `analysis` bump. A short `read_at` mid-document is a fault, not EOF.

Commit only after the new value exists: hist after `tree.replace` (reserve coalesced bytes before, commit after); clip after a successful cut; path + `saved_head` after a prepared rename. Rollback failure is `RTX_ERR_UNRESTORABLE` and sets `d.broken` — further edits refuse. Clipboard allocs into a local, then assigns. Empty source is a real clear. A path that gives up is unchanged or `broken` — never a hole that looks retryable.

## Faces

`@typehooks` / `@typeview` sit next to the types they name. A `@typeview` is the application’s allow-list. An `as:` embed retries UFCS on the inner type when the face grants the name. A face fences callers that take the face, not every `RtxDoc *` in the same file. Field writes and `as:` embeds are not gated unless the parameter is the face. Add the next face when a new function would otherwise take `RtxDoc *` and only need a slice — not a suite of faces because the type is wide.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — miss on the outer retries on the embed.
- `RtxDocHighlight` — `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl(RtxHlWin)`. It cannot `len` / `line_*` / `insert` / `type` / `save`.
- `RtxDocLayout` — measure may `len`, `line_count`, `line_start`, `line_guess`, `index_covers`, `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl(RtxHlWin)`, `fold_covers`. It cannot `line_of` / `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

Mark motion and fold walk the runs `ensure_hl` already produced. They do not lex ahead, pump, or keep a file-shaped table. Heading pairs use those runs; brace pairs (`{}` `[]` `()`) match on the caret’s 256KiB analysis page plus at most one neighbor page each side (same grain as `RTX_HL_WIN_MAX`, not the 64KiB store). Paint does not `ensure_hl` that span — skip uses whatever runs the layout window already has. A fold is stored only when both ends are in that window. Layout skips interiors; caret and scroll jump to the fold edge; hex ignores folds.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for `write_fd` / page-store internals.

## Edits

A same-file split is two cameras on one document. After a mutation, reparse once and drop every matching camera’s vis-row epoch — a width/top cache is not an edit stamp.

The document write is `replace` (byte range in `[0, len]`). `insert` / `erase` on the doc call it. User changes are `replace` on the history stack. Dirty is `hist.head != saved_head`. Offsets, caret, and selection are bytes. Save streams pieces (`write_fd`). Deleted bytes stay in the original/add buffers; hist stores them inline only while they still coalesce (typing/backspace), and otherwise keeps piece descriptors so undo splices the range back.

Save is a **safe rename**, not inode preservation: sibling `path.tmp.XXXXXX`, stream pieces, keep `0777` mode bits from `stat`, fsync the file, `rename` over the path, best-effort directory fsync. That replaces the inode. Hard-link identity is lost; a symlink at `path` is replaced rather than followed; owner, ACL, and xattr are not copied. That is the intended 0.1 policy (crash-safe replace). Do not write through the existing file. There is no mtime / inode check against an external change.

## Encoding

Offsets, caret, selection, and the piece tree are bytes. Text views walk
**clusters** for motion, wrap, hit-test, backspace, and measure (a UTF-8 scalar
plus following combining marks / variation selectors). Hex is a paint of that same editor, not a second one: caret, selection,
and unlock stay on the camera / document. Grid is the same: one record
per newline whose window-lex face is not STRING (the grammar's begin/end,
via lookback — not a CSV quote walker). No lex: every NL is a record.
Line 0 is a sticky header (always `read_at(0)`, not `line_start` from a
gap). Widths are header ∪ this fill (max line in a cell). Each column
caps at `RTX_GRID_MAX_COL`, not the pane; extra fields use `left_col`.
A cell wraps on its width and on embedded newlines. A delim inside a
string is not a field. The line prefix is still newlines. A mid-field
camera walks back to the previous unheld NL (lookback / scratch cap);
a miss is leftover, not a record index. Wheel/scroll steps whole records
(not vis wraps). Each wrap row keeps its physical line in the gutter.
`l` cycles default / wrap / hex / grid. A split is two editors (same
file or another path); focus is which pane. Hex motion already steps one
byte (and resets to the high nibble). On a hex pane, `type` accepts only
`0-9a-fA-F` and overwrites that nibble. The dump is display (UTF-8 lead,
continuation ·, else .), not a caret of its own. Leaving hex with a
collapsed caret snaps it to a cluster start. `read_at` stays bytes.
Invalid bytes are one-byte clusters (U+FFFD, width 1). Full UAX #29 graphemes
(ZWJ emoji, flags) are later.

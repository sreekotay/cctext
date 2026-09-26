# Design

One document core, two frontends (`cctext`, `cctext-ui`). Memory is owned or it is a view. Lifetime is a field, not a protocol. The document type is as wide as the domain — a face is reach at the call site, not a smaller struct. Epochs say what dies when; faces say what this call may do. A TU’s write is the function that accepts only legal values for that unit; that is what the header exports. Ownership is handled at the call site.

ccc enforces Result use, dest-live join, and face ownership. Product
shapes (one write, window lex, markup arity, no Vec on hist.recs,
cooperative pumps) are author law in this file and FRICTION.md.
Application policy (pair join, apply) sits on `replace` and is not a
language type — Rich panes reach it through the buf helper; do not treat
`replace_join` as a second write. Leaving arena / dest-live surfaces for
malloc is a regression unless the scratch dies with its arm.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change. A constructor does not tear down a live object. Reopen is two lines:

```ccs
d.destroy();
d.from_path(path) !>;
```

or a second local (`RtxDoc d2 = {0} @destroy`). Same for tree, layout, buf,
workspace. A buf slot destroys layout, then doc. `memset` is neither.

Hooks and faces live next to the type they name. A file cut is not an API — chapters of one TU stay chapters (`piece_tree_rb` / `piece_tree_lines` are not their own products). `workspace.cch` / `layout.cch` / `document.cch` / `ui.cch` are leaf TUs. `edit.cch` / `tm.cch` stay chapters of the document TU (FRICTION.md). The header exports the legal write (`replace`), not `insert` / `erase` because a rope usually does. Do not split `RtxDoc` into history/selection types because it is wide. A label on a shared record is an enum (closed dispatch, no `default:`); a value that is one of several payloads is a `@variant` — do not variant-shape every classifier. A Layout parameter cannot `insert` — that is local in the signature, not a comment. Frame copies take a scratch arena the function owns; analysis copies take `d.analysis`. Hist and path stay on `session` across reparse.

There is no inflight counter and no drain-to-zero. A path that gives up says so at the position that caused it.

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Document | piece-tree arena + page-store arena (fds + LRU page pool) | `RtxDoc.destroy()` |
| Session | `d.session` | close (path, undo) |
| Analysis | `d.analysis` | `analysis.reset()` on reparse |
| Workbook | `d.derived` → `RtxWb` (`core/wb.ccs`, `*.wb.md` only): own heap arenas for the model (names, cells, ASTs, graph), values, and per-operation scratch | a structural edit (or garbage past its bound) rebuilds the model; cell / calc-line / prose edits patch it; `RtxDoc.destroy()` |
| Find | `d.find.store` (query + hits) | new query resets; `RtxDoc.destroy()`; edit invalidates offsets |
| Layout | `L.store` | width/edit reset (vis rows) |
| Workspace | `w.session` | close (bufs) |
| Clip | `w.clip_a` (own heap arena) | next clip value (built on a fresh arena, then adopted); unchanged bytes skip the copy; close |
| Browse | `br.store` ents + `br.walk` jobs | kick resets; drop destroys |
| Project index | `RtxProjIdx.store` (fixed-size file / dir / name chunks that never move) + the walk's arena (jobs, kept ignore levels, visited set) | open / close; a re-walk builds a second index and swaps it in (search joined first); the walk arena dies at the join |
| Project search | `RtxProj.ps.store` (query copy, program, hit / group / preview chunks, the index candidates) | a new query or option resets; the index swap re-runs it; close |
| Search index | `RtxProj.si` (`RtxSi.a`: the file's bytes — filters included —, per-file identity + trust + first unit, the (dev, ino) table, per-unit word offsets and newline counts); the build's heap (per-file identities, per-segment unit lists, per-shard 2 MiB dedupe sets and read buffers, the file image) | loaded by the first search arm, kept for the session; a finished build frees it (the next search loads the new file); close |
| Quick-open | `RtxProj.pick` a / b (ping-pong: this query's top list and candidates / the last query's) | each non-extending query resets one side |
| Safe | on-disk journals (`~/Library/Caches/cctext/safe` or `$XDG_CACHE_HOME/cctext/safe`; `RTX_SAFE_HOME` overrides) | `RTXS` hist + `RTXC` state sidecar + `.b` base pin while dirty; `RTXW` workspace per project (`w/<hash of the git root, else cwd>`, and its `.si` sidecar: the search index, see Search index; the pre-v4 global leaf migrates once; the one open_files owes lands on the first safe_pump, after the first frame); each leaf is built in memory and written once before its fsync + rename; unknown ver / bad sum ignored; identity mismatch tosses clean hist, replays dirty hist onto its pin, else holds it; quit-`q` drops dirty journals |
| TM | process `rtx_tm_store` (langs + rules + interned pattern strings + compiled regex programs) | first lookup scans the grammar dir for metadata (name / scopeName / fileTypes); a grammar's rules load + compile the first time a lookup hands it out (under a lock: browse previews open on a worker); process |
| Frame | `cc_arena_stack` | end of the call (row / replace / copy) |

Document’s page store is that epoch’s bytes (fds + LRU), not a peer
epoch. Find / Layout / Workspace / Browse / Safe / TM / Frame are
caches and stores — not kinds of camera. Find’s listing arm gives each
lane cache replica a heap arena for the block window and hit offs
(~4 MiB). Island’s block loop and browse’s per-job scratch (ents/kids)
use per-iteration heap arenas for their windows (browse grows by
alloc+copy on that arena, same as published `br.ents`). Neither is the
find store; published find hits stay on `d.find.store`.

Analysis `secs` / `runs` / `run_top` / `tm_ckpt` and layout `rows` are Vecs on that
epoch arena. Hist restore text / ins are owned `Vec::[char]` on session.
`find.hits` / `browse.ents` are Vecs on their epoch arena (append in
`@stage`). Do not Vec `hist.recs` — rows hold those payloads; keep raw +
`cc_arena_realloc`. `browse.hold` stays raw on `hold_a`. `ws.bufs` is a
table of `RtxBuf *`, each allocated once.

## Interactive

One pane is one **camera**: **seeking** (`seek_off` / `fill_off`) or
**lined** (`top` / `fill`). Do not mix those window writes. The same
file in several panes is several cameras on one document (multiview),
not a third mode.

**Panes.** A pane is a slot (0..`RTX_PANES_MAX`-1, stable while it
lives): the buffer it shows (`pane[slot]`), its browse and preview
(`nav` / `prev`), and a camera. A split tree (`RtxPaneNode`: a leaf names
a slot, a split lays its two children side by side or stacked at a
per-mille ratio) places them; `order[]` is the leaves left-to-right /
top-to-bottom and is how every host walks the panes. Rects come from the
tree (`RtxWs_pane_rects` with the host's gap: `" │"` / a `─` row in a
terminal, pixels in cctext-ui), so drawing, hit-testing and divider drags
read one layout. Browse and its preview are per slot; `Ctrl-B` acts on the
focused one. TUI pane painters write pane-relative rows and
`g_cctext_row0` shifts every cursor move and the noted caret cell; the
input side subtracts the pane's row before the old single-pane hit code
runs.

A buf's camera fields (`top`, `seek_*`, `left_col`, the `RtxLayout`, …)
belong to one pane that shows it — `b->cam_pane` — and that is always the
focused pane when it shows the buf, because every `RtxBuf_*` motion and
edit reads and writes those fields. Any other pane on the same buf keeps
its own camera and layout in `cam[slot]` (`of` = the buf); `hold_pane`
swaps it onto the buf for a paint or a refresh and `drop_pane` swaps it
back. `RtxWs_panes_fix` restores that after anything moves (focus, split,
close, a pane switching buffer): a camera on a buf its pane no longer
shows is dropped, a pane newly on a shown buf seeds its camera from the
owner's, and focus takes ownership by swap. The journal (`RTXC`) holds
the buf's camera, i.e. the focused pane's; the others live for the session.

**Buffers.** `ws.bufs` is the tab order. Closing one destroys it, shifts
the pointer table (the struct is reused by the next add) and renumbers
`pane[]` and transaction members; a closed member leaves its
transactions. The panes that showed it move to the nearest tab no other
pane shows; the last close adds an untitled buffer. Dirty close is the
host's question (`ask_close`: save / don't save / cancel); don't save
drops the journal.

The UI thread does not wait on the line index. Open, paint, hit, select,
arrow, wheel, `%` jump, and a gap edit are bytes: `line_floor` /
`line_next` (local 8KiB), `fill_off`, and the **window** highlight face.
Gutter is `+N` / `-L` until the island meets the prefix (**cover**).

| mode | origin | fill |
|---|---|---|
| seeking | `seek_off` | `fill_off` |
| lined | `top` | `fill` |

Unwrapped text also keeps `left_col` (window x, same units as the
layout width). End, a caret past the pane, wheel left/right, Shift-
wheel, a tilt wheel, wheel on the bottom bar, or a drag past the pane
edge moves it. Cursor/VS Code drop Shift+wheel `deltaX`; the TTY
recovers it from the session while focused.
Wrap and hex stay at 0. Grid is the same offset, not a second mode.
The bottom bar appears only when a vis row is wider than the pane.

`line_of(seek_off)` hands off to lined (`top` / `fill`).
`index_covers` alone does not — that wrote `line_guess` into `top`.
Unlabeled stays seeking.

| window | caret already visible | else |
|---|---|---|
| `reveal` (follow) | keep | scroll / `seek_set` |
| `land` | snap that line to top | `seek_set` |

Click, type, and `ensure_caret` follow. Jump `%` lands. Find hits land
only when the user selects one (click / next / prev); the hit carries
its exact line (the scan counted it), so that land keys the gutter
origin on the hit's floor and plants no island. Apply and **pump**
keep the camera. Wheel and the scroll rail stay live while find is open.
`if (seek) land else reveal` is the bug: seeking is a camera mode, not
“index incomplete”. After handoff, wrap follow owns `top_wrap` again.

`line_of` is a covered lookup. Uncovered is an error — it does not scan.
`RtxDocLayout` does not grant it. Box select is two floors and columns,
not line numbers. `line_count` is soft until EOF. `line_start` may extend
the prefix; do not call it while seeking without cover.

**Cover** — the prefix grows from 0 (`line_scan_off` is the paid cursor;
the host does not pump it). An island grows from a land and meets the
prefix. `index_covers` is the test.

**Gutter key** — a live origin (`mark_off` / `mark_line`) plus `seek_rel`,
and up to 32 fraction pins. Once keyed, local scroll paints absolute
`L` without waiting for cover. Do not kick an island just to paint
numbers when a pin can re-key. (`pin_view` on the buf is a follow-lock
after wheel/bar — not a gutter pin.)

`g N%` is a byte snap plus island (backfill). Land on an uncovered
camera plants the same dest-live walk. The walk stops at the prefix
or the last planted 1/32 pin ahead of it — after an EOF update has
filled the slot table, that is at most one slice. `g L` counts from
the nearest exact floor at or below L (prefix frontier, live origin,
or a planted pin). A pin-seeded jump does not claim prefix cover for
the gap; on land it keys the gutter origin to that exact floor so
`seek_set` does not clear a mid-file mark and kick an island back to
BOF. An edit wipes pins at or after the byte and clears the
origin (BOF slot 0 stays), unless the edit is past the origin and adds or
removes no newline: then no line number moved, and a letter typed below a
keyed camera does not narrow the gutter (and re-wrap every row) until an
island re-keys it. After open the host does not walk toward
EOF. A far seek (>8 KiB) drops the origin and goes back to
red `+N` / `-L` unless a planted 1/32 pin is in that slice — then
that pin re-keys the origin. Slot `i` is byte `i * len / 32` (vacant =
`(size_t)-1`). Prefix cover or a connected island plants a pin and
keys the origin (island does not have to sit on a slot). The island's
lanes count each crossed slot's LFs to the anchor with their block;
connect plants from those counts and never recounts the span on the
host (that was ~150 ms of UI thread per 100 MB). Highlight /
markup still `ensure_hl` the **window** (vis rows)
plus `RTX_MARKUP_LOOKBACK` (and one neighbor page for pairs) — not a
walk to BOF. The markup lookback pass runs only where the lens may apply
(`markup_near`: a MARKUP or grammarless PROSE section); a code grammar
lexes the window alone. Fold is stored only if both ends are in that grain.

**Page motion** lays out once. PageUp / PageDown (`page_vert`) walks the
pane's own rows when they hold the caret and the target (laid out at
this width and edit stamp), else one scratch fill of the caret's line and
the page past it; unwrapped it steps line starts and lays out only the
landing line. It lands where n `move_vert` calls land (`scroll_smoke`),
Rich tables included: a table's separator line is never a landing, a
record whose cells wrap is stepped one wrap line of the goal cell at a
time, and a motion fill classifies every table it touches (measured whole,
as the pane measures it) — not only the ones the pane has laid out. A
fill that holds `RTX_MD_TABLE_FILL` tables may have dropped one: the page
then steps as `move_vert`.
A motion's scratch fill lexes with `rtx_hl_keep` (`RtxLayout.hl_keep`):
it never replaces or shrinks the pane's window lex. (One `move_vert` per
row was ~100 fills a page, each a one-line window that shrank `hl_cov`,
so the next line relexed its lookback: 100+ ms pages on Markdown.)

**Run working set.** A window lex that moves on past `hl_cov` resumes
that lex from its last checkpoint at or below its end (the end one only
when its last line was whole) and lexes the new bytes — the result is the
one lex from the same anchor, joined at the seam as an edit relex joins
it; a camera fill then drops the TM runs above its window in the lex
region (the window sections with no `===` header between them). After
each frame's layout the workspace trims every doc to its cameras
(`RtxWs_hl_trim` → `RtxDoc_hl_trim`): runs farther than
`RTX_HL_KEEP_MARGIN` (one window grain) from every pane window on it and
its caret go — a lexed section whole or not at all — once the run count
has doubled since the last trim (floor `RTX_HL_TRIM_MIN`). The window
cache entries, sections (`hl_done`) and `hl_cov` that planted them forget
it; checkpoints stay, so a camera that returns relexes from them. A full
checkpoint table drops every other one in its lower half. Clips read
only runs that can overlap (`runs_sorted` / `runs_maxlen` bound the
sorted prefix, then the lex's unsorted tail): nothing to drop is
O(log n).

`lf_ready` the field is a latch. `lf_ready()` / `index_covers` are
false when the flag is stale (huge + `subtree_lf==0`). The flag
means node newline weights match the body (one unedited original at
EOF, or a small recount). It is not “the prefix reached a milestone.”
A split tree at EOF stays progressive: `scan_line` is exact, weights
are not. Setting the flag with `subtree_lf==0` made `line_count()==1`
and snapped the camera to line 0.

The prefix cursor is paid work. A covered edit patches it
(`note_insert` / `note_erase`). Truncate at the caret only when the
edit changes newlines (Enter may park the scan). A letter key that
parked `line_scan_off` at the caret rescanned the tail.

`line_count` is a scroll budget until weights are live. A window
write must not take `to = len` because that total said “last page.”

Host frame: mutations, then one window write. Both hosts are
`input → pump → layout → paint` — **pump** is the host verb. TUI read
blocks; timeout 0 while dirty so the first paint and the frame after a
key are not a 400 ms wait. Do not layout, then handle, then layout
again — a command that forgets a dirty bit paints the old camera.
Both hosts sleep until input, the next blink (cctext-ui; cctext only
with `--caret=cell` — the terminal blinks its cursor), a Safe debounce
(`safe_next_ms`), or the next overlay tick of a scan that still
publishes (`live() && !done`, island pumping — not `live()`, which
stays up until a join nobody makes); with none of those due they block
until input. A young find is polled at 250 µs, then 2 ms. The
cctext-ui wait is not a window turn: scans pause for layout + paint and
for each toolkit Draw (`on_draw` brackets itself), and publish while
the host sleeps.

Walks go forward: a fill, a hit / `x_of`, and a pane paint each carry
one `RtxStyleCur` (the runs covering the last offset) and one
`RtxHideCur` (hidden-hint edges); both equal `style_at` / `style_next` /
`hidden_to` at every byte (`style_cur_smoke`). The TUI pane builds each
row in one buffer with SGR deltas, and a row the layout kept
(`RtxVisRow.gen`) with the same style signature, no caret / selection /
pair end and no Rich marks replays last frame's bytes.

The caret is its own layer; text paint never draws the editor or a
prompt caret (`RtxUi.caret_host`: `rtx_ui_here` is 0, the prompt caret
string is empty). The painter notes where the caret is
(`rtx_ui_caret_at`) as it paints that spot, so the layer sits on the
painted glyph by construction — wide clusters, hidden Rich hints,
stand-ins, MD table padding, grid cells, `left_col`. `--caret=cell` (or
`RTX_CARET=cell`) is the painted fallback, and a toolkit with no layer
gets it too.

- **cctext**: the terminal cursor is the caret. `cctext_hw_mark` records
  the cell; after the row diff, inside the same DEC 2026 update, the
  frame hides the cursor while rows are written, then CUPs there and
  shows it — only changes are written, so a frame that moved nothing
  writes nothing. DECSCUSR 5 (blinking bar; 1, a block, on a hex nibble;
  6 / 2 steady for `--no-blink` or an unfocused terminal). The terminal
  owns the cadence (`RtxUi.host_blinks`): no blink timer, zero wakeups.
  Find / jump / the browse glob own the cursor while they have focus;
  help, apply, stats and the ask prompts hide it, as does a caret off
  screen or in the unfocused pane. Hex's text-column mirror stays a
  painted (solid) cell — one cursor. The leave string (exit, fatal
  signals, ^Z) resets the shape (`CSI 0 SP q`) and shows the cursor.
- **cctext row diff and scroll regions**: the frame is captured, split by
  CUP into one log per screen row (`cctext_frame_split`), and
  `cctext_frame_publish` writes the rows whose log changed. A pane that
  spans the terminal's width (`cctext_draw` notes its rows) is checked
  for a vertical shift first: a row log's normal form drops the CUP row
  numbers (a row log only CUPs into its own row), and the shift k whose
  moved rows keep the most bytes wins if that beats the rows kept in
  place by more than the escapes cost (with fewer than 3 changed rows
  the search is skipped). The terminal then shifts: `DECSTBM top;bot`,
  CUP to the region top, `DL k` (up) or `IL k` (down), `CSI r` — DL / IL
  at the region top are SU / SD, and pyte models them (it has no SU /
  SD). The diff runs against the shifted old logs (exposed rows are
  blank, stale rows are rewritten), so only the exposed rows and rows
  whose gutter, rail thumb, caret or marks changed are written; the
  cache is the new frame. Side-by-side panes share terminal rows — a
  region scroll would move the neighbour — so they keep the plain diff
  (DECSLRM under DECLRMM is not used: support needs a DECRQM round trip
  and pyte cannot check it). All of it sits in the frame's DEC 2026
  update; the leave string (exit, ^Z, fatal signals) carries `CSI r`.
  `--no-scroll-regions` / `scroll_regions: false` /
  `RTX_SCROLL_REGIONS=0` / `TERM=dumb` turn it off. The busy spinner's
  end does not erase the status row in a write of its own: it marks the
  row stale and the frame (or a chrome paint the loop asks for) rewrites
  it in the same write.
- **cctext-ui**: during a whole-area Draw the painter hands each caret bar
  to `gui_caret_bar` (clipped to the scissor; hex has two, fields one);
  after the Draw `ui_os_caret_set` moves the layer and `ui_os_caret_show`
  is the blink phase — no layout, no Draw of text. GTK: an overlay child
  over libui's area (input pass-through); GTK 3 keeps no retained pixels,
  so the layer paints its off phase from the pixels the whole Draw left
  under it (`ui_os_caret_capture`), and the area Draw GTK runs under the
  layer returns at once in ui_plat (clip inside the layer, "layer-only").
  A native X child window would skip even that Draw, but GDK clips the
  parent's paint by it (a frame moved over its old one keeps stale
  pixels) and places it a move late on resize — not used. AppKit:
  layer-backed subviews of the area view (untested). Win32: the system
  caret (`CreateCaret` / `SetCaretPos`; the OS blinks it) — documented
  in the stub, which still paints. rect 0 is the IME anchor
  (`gtk_im_context_set_cursor_location`; `firstRectForCharacterRange`
  later). `--caret-fade` eases the phase over 100 ms.

The GUI caret blinks 530 ms on / 530 ms off (`RTX_UI_BLINK_MS`; Windows'
`GetCaretBlinkTime`), and only while it is being used: input or focus-in
starts it, and `RTX_UI_BLINK_IDLE_MS` (10 s, GTK's
`gtk-cursor-blink-timeout`) after the last input, or on focus-out, it
settles visible and its timer stops (`rtx_ui_blink_next_ms` is -1).
`--no-blink` never blinks. `RTX_BLINK_IDLE_MS` overrides the timeout
(tests). A blink turn is not a frame. With the painted fallback it
repaints the rects the last whole-window Draw noted (`gui_blink_note` —
the caret's row across its pane, a prompt's line) with no layout, and the
painter skips every row and bar outside a Draw's clip. The rect is a row
band, not the caret box: a glyph cut by the clip edge rasterizes
differently from the same glyph drawn whole. Any other change that turn,
or a caret signature that differs from the last whole paint (moved caret,
camera, prompt, pane, size), paints the window instead.

Shared UI (`ui.cch` / `ui.ccs`, `ui_cmd.cch`, buf motion on `RtxBuf`) owns modal
state, command ids (`CMD_*` / `rtx_cmd_from_letter`), field edits, and
hex/text Home-End / up-down. Each host maps its events and draws.
Pixels, CSI, fonts, and OS clipboard stay in the frontend. Do not
extract a host protocol — a letter map and a buf write are the cut.

**Command table.** `cmd.cch` is the one list of commands: id, title,
category, `CMD_*` (or a `run` hook over `RtxCmdCtx`), default chords per
host, `Esc` letters, `need` / `deny` context bits, GUI menu, help notes.
Nothing else lists commands: `rtx_cmd_from_letter` is its letters column,
the help overlay is generated from it and the live keymap
(`rtx_help_rebuild`), ui_plat.c builds the menus from its `menu` column,
and the palette (`palette.cch`, state in `RtxUi.pal`) ranks its rows with
`fuzzy.cch`. A chord is `key | mods` (Cmd folds into Ctrl); the host turns
an event into one — the TUI decoder sets `CctextKey.chord`, the GUI
`gui_chord_now` — and asks `rtx_keymap_lookup`: keys.json entries first,
then table defaults (a `^` chord claims over older rows once its row is
available), then a letter chord drops Shift. NONE keeps the host's own
meaning (plain keys, arrows, the decoder's legacy map); INERT is an
unbound or unknown id. The host still dispatches: a row becomes a key
kind (TUI) or a `CMD_*` (GUI), so modal states (find, jump, help, apply)
see the same events as before. Mode keys (find `Alt` toggles, apply
letters) are `LOCAL` rows: listed and runnable from the palette, not in
the keymap. A reserved row holds an id for a command another change adds;
a real row with that id (static or `rtx_cmd_register`) shadows it.

## Scan

Backfill is a pump. The host yields (`rtx_ui_work_pump`). Do not extract a
shared type — bits stay local (`find.done`, `browse.scanning`, GUI AST).
The next walk copies this table.

Active is the dest (`h.live()`). Results appear only at an explicit
site: `@stage`, `recv`, or return from the dest-live arm. Progress is a
monotonic cursor published at that same site. The frame does not read a
field the worker is still storing into.

| | start | one step | live | resume | deny |
|---|---|---|---|---|---|
| find | `find_set` | dest-live wrapper; wait-for 2 MiB blocks | `h.live()` | `scan_off` | — |
| jump | `want_kick` | `want_step` | `want_pumping` | `want_off` | — |
| prefix | — | — | — | `line_scan_off` | host does not pump |
| island | `isle_kick` (land / gap view) | dest-live wrapper; wait-for 2 MiB blocks | `isle_h.live()` | `isle_from` | — |
| browse | `rtx_browse_kick` | dest-live wrapper; one `RTX_BROWSE_WAVE` of dir jobs; pump joins a finished arm then starts the next | `h.live()` | `jhead` | — |
| proj walk | `rtx_proj_open` / `rtx_proj_rewalk` | one dest-live arm for the whole walk: waves of `RTX_PROJ_WAVE` dir jobs under `@parallel wait`; the stage appends dirs / files / child jobs and publishes `pub_nf` / `pub_nd` | `!pub_done` | — (a re-walk starts over) | — |
| psearch | `rtx_proj_search_set` | dest-live arm over index ids `[next, pub_nf)` at kick, one `@parallel wait` with a ticket per `RTX_PS_CHUNK` files (the lane reads each file whole into its reused buffer and keeps the chunk's hits on its cache replica; with the search index it stats first, skips an indexed, unchanged, trusted file none of whose units pass, and reads a big one only around its passing chunks for a one-line pattern); the stage appends one group per file, in id order, and publishes once per chunk; the pump kicks the next arm while the walk grows; the query's first arm loads the index and computes the passing units | `ps.wk` / `!complete` | `next` | binary / over size: counted, skipped; index-ruled-out: counted (`n_idx_skip`, chunk reads `n_idx_chunk`) |
| index build | "Build Search Index" / "Update Search Index" (`rtx_proj_index_kick_mode`; deferred while the walk runs) | one dest-live arm: two passes over the index files, each a `@parallel wait` over `lanes` shards taking walk-order segments from a counter, bits set straight into the file image, then write; an update stats the walk, re-reads dirty units as jobs and packs new files as a two-pass tail | `rtx_proj_index_building` | — (a rebuild / an update) | close and a re-walk swap cancel it (nothing written; a swap re-asks); the pump joins it while no search arm is live |
| replace | `rtx_replace_kick` | `rtx_replace_step`: 1 MiB windows of starts on the UI thread, up to `RTX_LINE_ISLE_WORK_MS` or input; past the bulk threshold each site also feeds the bulk build (tree reads, staged add bytes) | `rtx_ui_repl_busy` | `pos` | long match / cap / an edit since kick → `RTX_REPL_FAIL`, nothing edited |

The project walk and search copy the find / browse shape. A walk lane
reads only its job, the walk's constants and index entries below the
published counts (chunk pointers are set before a count covers them, so
the host reads ids below `pub_nf` while the stage appends); it parses its
directory's `.gitignore` / `.ignore` onto its own scratch, and the stage
copies the rules the children need onto the walk arena. Quick-open scores
on the UI thread (parallel shards past 16k files, an immediate-wait
`@parallel for`); a query that extends the last one rescores its
candidates only, and the pump folds newly published files into the top
list. Search offsets are the file on disk, not a buffer. A search ticket
is a chunk of files, not one file: per-file tickets spent more on the
turnstile hand-off (enter, stage handshake, fiber switch) than on a
~13 KB file. A chunk's lane stops once it holds more than
`RTX_PROJ_HITS_MAX` hits; the stage caps before the file that would pass
it, so a group is never cut. `rtx_proj_search_finish` (batch, tests)
does not wait for the first walk: like the pump it kicks arms over what
the walk has published (reading ids below `pub_nf` only), each once a
pipeline's worth of files (`RTX_PS_CHUNK` × 2 × lanes) is past `next`,
and joins the walk at the end. Walk and search kick `rtx_par_pool_up` (the
runtime pool to one worker per core) before their wait-for: the tickets
block in open / read / getdents, and the scheduler's own growth reads
turnstile parks as I/O multiplexing and stays at 2 workers.

A lane reads the tree (node walk + `pread`); pause gates only the write
stage. So every tree mutation joins the island first (`replace`,
`splice_refs`, `append_copy`), the doc joins find before its `replace` /
undo / redo, and `destroy` joins find before the tree goes. The edit
patch (find shift / seam) runs after; the next pump restarts from
`scan_off`. Host readers take the published hit count
(`rtx_find_hits_n`), not `hits.len`.

Kick plants the first paint and returns. A dest-live kick has no
"not yet started" window — the arm may finish inline before the next
statement. Everything the finish path reads (browse keep, jump dest,
find query) is set before the kick, not after. `RTX_PARALLEL_INLINE=1`
(`./make.shcc @smoke_inline`) runs the browse and find wrappers on the
caller so that schedule is the default in CI. Island stays dest-live:
seeking without cover is a live prefix, not a drained result.
Browse, find, and island are dest-live so the frame pauses / resumes /
cancel+wait and does not `.wait()`. `g L` stays one closed interval per
step; returning is the yield. Find and island are the sequential 2 MiB
block loop with `@parallel wait` / `@stage`. Browse is the same loop
over directory jobs: collect is ticket-local, the write stage appends
ents / child jobs and walks `jhead`. Workers read the query copied into
the walk at kick (`RtxBrowseQ`), never `br->dir` / `br->glob`; the job
cap is queue depth, not jobs ever queued. A lane reads the wave's copy
of its job and of `deep_phase` (`wk->wave`, taken before the lanes
start), never `wk->jobs` — a stage's push moves that table. While the
arm is live the host reads only the published view
(`rtx_browse_view_snap`) and `rtx_browse_scanning` / `_truncated` /
`_denied`, never `br->ents` or the flags; du and retarget run between
waves (after the join, before the next kick). Gutters stay `+N` / `-L` until
they meet the prefix. `isle_step` kicks if the handle is down — the
host does not walk the island on the UI thread. Kick `@serial` is empty
so the workers are the scan. Query copy and hit offsets and lines stay on
`d.find.store` and die with the document. A longer prefix query filters
hits and keeps `scan_off`; a cap resumes from the last accepted hit; a
shorter or non-prefix query resets. The filter runs only when it is
semantically valid (literal, same options, not whole word). The query
compiles on the rx engine (literal unless regex; options case / word /
regex, Alt-C / Alt-W / Alt-R). A search is the lazy DFA (a dense
state × byte-class table per lane scratch) behind the rarest required
literal (prefix, inner literal on its line, or an alternation's set;
ignore case too); a literal query is that filter alone (core/rx.cch).
A pattern with none of those (its literal sits after an unbounded part
that may take a newline: `\w+\s+Holmes`) may split at the top level as
P L S (reverse inner; reverse suffix when S is empty): the filter finds
L, P reversed gives the leftmost start of a P that ends at the hit, and
the forward DFA anchored there confirms it with the usual leftmost-first
end (both lazy DFAs have their own cache and budget, and give up to the
plain path when it thrashes). Compile builds it only
with a proof that no earlier match can use a later hit (`rx_ri_safe`: P
of one length; P never takes L's first byte; a required class run ends
P that no other part of P and no first scalar of L shares, `\s+` in
`\w+\s+`; P a chain of class runs, `.*` or `\w+\s*`), so the first
confirmed hit is the plain engine's match. Linear time: a hit inside the
last failed forward scan, a reverse scan still alive below where that
scan stopped, reverse + failed forward bytes over 3x what the hits
advanced, or hits every few bytes (counted over the calls on one text)
give the call to the plain DFA from
`lo` and rest the strategy on that text for 1 MiB. Reverse scans never
read below `lo`. A window that is not the text end never guesses: past
the last whole hit, a reverse scan from the window end for a prefix of
a match (every consuming pc of the reversed pattern) names the leftmost
candidate the edge cuts, and the plain DFA answers from there (a
leftover, as before). Texts under 256 bytes (grammar lines) stay on the
plain DFA; `force` 4 runs it on any text (`rx_conform`'s fifth engine,
`rx_smoke`'s differential and linear-time checks).
Text is walked in units: a scalar, or a stray byte (a lead with fewer
continuation bytes than it needs is one unit, cut where they stop). `.` and
negated classes take one unit, never a scalar's bytes one by one
(RXA_NCONT: the stray path asserts no continuation follows), and a match
starts only on a unit boundary (the DFA's unanchored loop steps units;
Pike / backtracker test `rx_unit_start`; a reverse-DFA start inside a unit
defers to the Pike VM). `rx_conform` runs the Rust regex crate's test data
(tests/rx_conformance, translated to the Onig flavour) through every path
and lists the deliberate differences: Onig empty-iteration and named-group
captures, ASCII \d, the \w policy, no ASCII / byte mode, the fold
table. The prefilter's byte table rates non-ASCII for text in the query's
own script (leads common, continuation bytes from ru / zh text).
Literal hits overlap; regex hits are the
leftmost-first chain. Each block searches its 2 MiB plus a
`RTX_FIND_SPAN` (64 KiB) overlap; `@stage` re-chains across block edges
so lanes equal a sequential scan. A match that would pass the span is
not guessed: `long_n` / `long_off` show it in the header. A regex edit
re-scans a span-sized window and re-chains until it meets the shifted
hits. `find_apply` plants via `find_set`.
Newlines are counted in one place: `core/lf.ccs` (`rtx_count_lf`,
`rtx_nth_lf`, `rtx_last_lf`; AVX2 picked at run time, else SSE2, NEON
on arm64, SWAR elsewhere; `lf_smoke` checks every path against a byte
loop). Never a byte loop or one `memchr` per line on a scan path. A find
lane counts the block while it reads it (256 KiB pieces, in cache) and
counts hit lines between successive hits; the prefix, jump and rebuild
scans hop stride slot to stride slot with `rtx_nth_lf` (`rtx_line_eat`)
and pread straight into their buffer (`read_uncached`), not through the
page LRU. The pread copy itself stays: an mmap of the original would
SIGBUS when another process truncates the file.
`find_pump` kicks if the handle is down (edit invalidate) and marks the
nearest hit. No list row is current until the user selects one; the
camera stays. Chrome that owns the walk shows `scanning...` / `capped`.
Find lists `scan_off` as `N%` and `off/len` bytes. TUI wraps only when
the camera moved; help / find / jump paint as chrome on the last
window. Tests call `finish` (wait). Do not drain the first screen
before first paint. Tests that need a covered `line_of` call
`rtx_line_scan_to` (or a pump) first.

### Search index

Project search reads every file, so a repeat search over a big tree costs
what reading it costs. The search index (`core/sindex.ccs`) is one Bloom
filter per 64 KiB of the project — a block of consecutive files, or a
chunk of a big file — about 2 % of the indexed bytes, built only when
asked ("Build Search Index" / "Update Search Index" in the palette; batch
`project-index [--update]`). With no index file the search is exactly
the plain scan: the plan, the loader and the lanes' stat never run
(`rtx_si_work()` stays put; `sindex_smoke` asserts it). The model was
picked by a simulation on the Linux tree (ripgrep's benchsuite queries
plus a dozen identifiers): a truncated rare-trigram posting index (the
previous design) read 100 % of the bytes at the median query at 1 %,
because every trigram of a common identifier is in hundreds of files;
per-block Bloom filters read 37 % (128 KiB blocks), 24.5 % at 64 KiB
when a big file is read only where its chunks pass, 12.8 % at 2 %.
Walk-order locality is what makes blocks work (shuffled: 63 %); k = 2,
a per-file second-level filter, dropping common trigrams and 4-grams
were all worse at the same size.

Units. The indexed files — regular, not empty, up to 64 MiB
(`RTX_SI_FILE_MAX`, or the search's size cap when bigger), no NUL in the
first 8 KiB — in walk order (paths compared with `/` lowest, so a
directory's files stay together) are packed greedily into blocks of up
to 64 KiB. A file over 64 KiB is its own run of chunks: chunk j is
`[j·64 KiB, (j+1)·64 KiB + 63)`. The 63-byte overlap is the longest
literal the planner uses (64 bytes, `RTX_SI_WINS` + 2) less one, so any
literal lies whole in some chunk. Each unit's filter holds its
ASCII-folded trigrams with one hash (k = 1: `bit = h32 · bits >> 32`),
sized in 64-bit words to the unit's distinct-trigram count so the
filters together fill what the budget leaves after the fixed parts.

Build: two reads of every indexed file. The walk order is cut into at
most 256 segments (independent of the lane count: one lane and eight
write the same file but for its build time); `lanes` shards in a
`@parallel wait` take segments from an atomic counter. Pass 1 reads each
file (identity from its fd), packs the segment's blocks, counts each
unit's distinct trigrams (a 2^24-bit set per shard, cleared through its
touched list) and each chunk's newlines. Then the budget
(`search_index_pct`, default 2 % of the indexed bytes, capped at 256
MiB) pays for header, root, device table, file table and unit directory;
the rest is split over the units by distinct count (the directory's size
depends on the word counts: sized twice). The file image is allocated
once; pass 2 re-reads each file, checks its identity against pass 1 and
sets the bits straight into the image (units own whole words: no
atomics). A file that changed between the passes gets the dead bit in
place (its stamp word keeps its size) and is always read.

Identity and trust: (dev, ino, size, mtime ns, ctime ns) of the fd read
in pass 1 — ctime because `touch -m` restores the mtime but not ctime.
A file is trusted only if max(mtime, ctime) < build start − granularity
(2 s when a stamp has no sub-second part, else 100 ms; git's
racily-clean rule). Two paths on one inode (hard or symbolic links):
neither is trusted by a search.

Query: the regex engine's required literals (`rtx_rx_req_n` /
`rtx_rx_req_lit`, the prefilter's set — every match holds one of them;
ignore-case pairs and tiny classes as byte sets) become trigram windows
of up to 27 folded alternatives. A unit passes when, for some literal,
every window has an alternative whose bit is set (a literal under 3
bytes, or no required literal: no pruning, the plain scan). The first
arm computes the passing units once (a bit per unit; ~30k units on
Linux, well under a millisecond per window). A lane then stats each file
(`fstatat`) and, when its (dev, ino) maps to an indexed id whose
identity is unchanged and trusted and it is not a dirty buffer's file
(`rtx_proj_keep_add`):

- no unit passes: skipped unread (`n_idx_skip`);
- a big file with some but not all chunks passing, and a pattern no
  match of which crosses a newline (`rtx_rx_one_line`: no consuming
  instruction, look-around included, takes `\n`): read only there
  (`n_idx_chunk`), after re-checking the identity on the opened fd;
- otherwise read whole.

Chunk reads. Runs of passing chunks merge (and runs whose padded ranges
meet); each run `[cs, ce)` is read with 256 bytes of padding and widened
until it holds the `\n` before `cs` and the `\n` at or after `ce − 1`,
plus the byte after it — whole lines with one byte of real context on
each side, so `^ $ \b \A \z \Z` and look-around (which cannot cross a
`\n`) decide exactly as in the whole file; `\A` holds only at offset 0.
Matches may start only inside the run's lines, and a later run never
rescans what an earlier one did, so there are no duplicates. The line
number at the run's start is the chunk's newline count from the index
(`unl`, a varint per chunk in the unit directory) less the newlines
between the line start and `cs`. Counting forward instead would read the
bytes the chunk read skips; the stored counts cost 2 bytes a chunk.

Update ("Update Search Index", `project-index --update`): load the index,
`fstatat` every walked file and look its (dev, ino) up (one walk path
per old entry; a shared inode's entries are matched one each). Unchanged
and trusted by its stamps: kept, unread. Changed with the same shape
(small, or the same chunk count): its unit is dirty — the whole block
(≤ 64 KiB) or the whole chunk run is re-read and its filter rewritten at
its fixed size (fresh newline counts too). Deleted: leaves the table
(its bits stay; false positives only). New, or reshaped (small ↔ big,
another chunk count): packed into new tail units, sized at the build's
bits per trigram (in the header). More than 20 % of the units dirty or
new (`RTX_SI_UPD_PCT`), no index, or an unreadable one: a full build,
and the report says why. The new file's build time is the update's
start: a kept file was trusted before, so it is still trusted. A budget
change (`--pct`) needs a Build.

File: `<safe>/w/<FNV-1a 64 of the root realpath>.si`, the RTXW leaf's
sidecar, built in memory, written to `.tmp`, fsync'ed, renamed. 16
header words ("RTXI" + version 2, build ns, granularity, file / device /
unit counts, section sizes, block size and overlap, indexed bytes,
budget, bits per trigram, the last full build's ms); the root path
(checked on load); the device table; the file table; the unit
directory; the filters; a 64-bit four-lane word checksum. File table,
per file in unit order, no paths (lookups go by (dev, ino); the walk
supplies paths): the device index only when there are several, the
zigzag delta of the inode from the previous file's, the size, the
zigzag delta of the ctime seconds from the previous file's, a u32 of
ctime nanoseconds with `mtime == ctime` and dead flags in its top bits,
and the zigzag mtime − ctime only when they differ. Unit directory: per
run, `nfiles << 1` (a block) and its filter words; or
`nchunks << 1 | 1` (the next file's chunks) and, per chunk, its words
and its newlines. The loader checks every count against the bytes left,
chunk counts against sizes, block members ≤ 64 KiB and the word total. A
bad magic / version / root / size / sum: no index.

Measured on Linux (torvalds/linux, depth 1: 95,986 files, 1.64 GB
indexed; 4-core VM shared with other agents' builds, so times are
noisy — best of 3, interleaved; page cache warm; whole `cctext --batch`
processes, walk included; rg 14.1 `--hidden`; `bench/rg_suite.py`):

| | |
|---|---|
| index | 32.7 MB = 1.99 % (budget 2 %): file table 0.785 MB, unit directory 0.095 MB, filters 31.8 MB |
| units | 30,525 (15,313 blocks, 15,212 chunks), fill 0.32 |
| file table | 0.048 % of the corpus = 2.4 % of the budget (8.2 B a file); the previous table (paths front-coded, full varint stamps) was 3.45 MB = 0.23 %, 23 % of a 1 % budget |
| build, all lanes | 5.2 s (pass 1 2.2, pass 2 2.7, write 0.26), peak RSS 93 MiB |
| build, one lane | 13.3 s (5.9 / 7.1 / 0.27), peak RSS 78 MiB |
| update, 1 changed file | 0.51 s (stat 0.31, read 0.03, write 0.18): 1 unit re-read, 60 KB |
| update, 100 changed files | 0.46–0.65 s: 84 units re-read, 4.3–6.4 MB |

Bytes read per query, as a % of what the plain scan reads, against the
simulation at 2 % (its chunk-read column; its tree excluded the eleven
files over 8 MiB, which the bench also runs with `--max-size 8388608`
to compare; with them read — they are indexed and chunk-read too — the
bench's default run reads `PM_RESUME` at 24.4 %):

| query | cctext | simulation |
|---|---|---|
| `PM_RESUME` (literal, `-i`, `-w`) | 27.0 % | 24.0 % |
| `[A-Z]+_RESUME` | 47.7 % | 44.1 % |
| `ERR_SYS\|PME_TURN_OFF\|LINK_REQ_RST\|CFG_BME_EVT` (and `-i`) | 36.9 % | 47.5 % |
| `pthread_mutex_timedlock` | 0.7 % | 0.8 % |
| `crc32c_le` / `regmap_update_bits_base` / `iommu_map_sg` | 2.3 / 2.3 / 2.3 % | 1.2 / 3.8 / 3.2 % |
| `kfree_skb_list` / `hrtimer_forward_now` / `SLAB_HWCACHE_ALIGN` | 3.1 / 0.8 / 0.6 % | 4.5 / 1.9 / 0.9 % |
| `kmalloc_array` / `copy_from_user` / `spin_lock_irqsave` | 12.3 / 14.1 / 13.7 % | 12.8 / 10.8 / 14.4 % |
| "should never happen" / "for the time being" | 12.4 / 38.6 % | 16.6 / 33.3 % |
| absent `flibbertigibbet` | 0.4 % | 0.3 % |
| median of the 19 | 12.4 % | 12.8 % |

(The "about 16 %" at 2 % is the simulation's whole-file median; its
chunk-read median is 12.8 %. The alternation reads 10 points less than
modelled; the rest are within 4 points.) Queries with no usable literal
(`\p{Greek}` blocks, `\wAh`, `\w{5}\s+…`) are plain scans: 100 %.

Warm wall time (whole processes; walk = the project walk, which a batch
process pays each time and the editor once per session; lanes = time
summed over 4 lanes):

| query | rg | no index | index | walk / search ms | lanes stat / read+scan, index | lanes stat / read+scan, no index |
|---|---|---|---|---|---|---|
| linux_literal | 576 | 723 | 548 | 539 / 539 | 311 / 351 | 45 / 1364 |
| linux_literal_casei | 644 | 858 | 739 | 731 / 729 | 359 / 447 | 53 / 1446 |
| linux_re_literal_suffix | 744 | 1157 | 831 | 817 / 814 | 463 / 954 | 66 / 2034 |
| linux_word | 841 | 1084 | 796 | 779 / 779 | 487 / 505 | 97 / 1875 |
| linux_alternates | 958 | 1331 | 908 | 812 / 896 | 506 / 850 | 79 / 2679 |
| linux_unicode_word (plain) | 542 | 719 | 719 | 628 / 709 | 46 / 1256 | 45 / 1302 |
| linux_no_literal (plain) | 1669 | 3904 | 4288 | 2006 / 4254 | 120 / 13062 | 108 / 11930 |
| pthread_mutex_timedlock | 547 | 712 | 443 | 434 / 435 | 256 / 11 | 42 / 1296 |
| kmalloc_array | 583 | 738 | 483 | 471 / 472 | 286 / 157 | 50 / 1300 |
| spin_lock_irqsave | 660 | 726 | 497 | 481 / 482 | 260 / 162 | 70 / 1326 |
| flibbertigibbet | 924 | 1066 | 639 | 628 / 629 | 278 / 13 | 67 / 1697 |
| all 24 queries | 19.7 s | 25.9 s | 20.7 s | | | |

Reading: with the index, read + scan falls from ~1.3 s (summed) to
10–500 ms, and the search finishes as soon as the walk does — the walk
(0.4–0.8 s here, one `getdents` + `fstatat` pass) and one `fstatat` per
file in the lanes are the floor. In the editor the walk is paid once, so
the gain there is the read+scan column. Counts equal rg's except where
the semantics differ (a symlinked file rg does not follow, `\p{Greek}`
run as block classes, `\w` / `\s` past ASCII); with and without the
index cctext's rows are identical for every query (the bench compares
them).

Limits: a changed file costs one re-read of its block (≤ 64 KiB) or of
its whole run of chunks; updates never compact (deleted files' bits and
tail units stay until a Build); shapes are fixed, so a file growing
past 64 KiB moves to the tail. The residual risk of trusting stamps is
a change that keeps size, mtime and ctime (a clock set back, a file
system that does not move ctime, mmap stores never msync'ed), the same
as git's.

## Surfaces

Fallible APIs are Results (`T !>(CCError)`). Value returns are only pure queries of already-valid state (`len`, `has_sel`, `dirty`, …). OOM, IO, and a short mid-document read are errors — never a short slice or a zero that looks like success on a commit path.

`line_start` / `line_of` are `size_t !>(RtxIndexErr)`. `RtxIndexErr` is not a face of `CCError` — do not `@typeview { as: base; }` so save/edit handlers cannot swallow an index fault. Helpers that call `line_*` are `T !>(RtxIndexErr)` and pipe. A `!>(CCError)` surface that also does index work translates once.

One document read surface: `size_t !>(CCError) read_at(off, dest)` with `char[:] dest`. Success returns `got = min(dest.len, len - off)` (EOF clamp is success). A hole inside that range is an error. Callers that need every byte of `dest` require `off + dest.len <= len` (or check `got == dest.len`).

If an API returns owned bytes, the destination arena is the **last** parameter (receiver first, arena last). That arena *is* the product’s lifetime — the caller names WHERE. `scratch_span(from, n, a)` copies onto `a` when the range is not one piece. Call-local `@scratch` / frame stack stays inside the callee and is not returned. Views (`span`) do not take an arena. `RTX_FRAME_SCRATCH` is a stack budget, not a span cap.

A Result failure is **unchanged** or **`broken`**. `broken` means the tree may be inconsistent — set only after a mutating step that could not be rolled back. The latch is a kind (`RTX_ERR_UNRESTORABLE`), not a message. Pre-mutation faults leave the object intact and do not set `broken`.

Hist stores everything the next edit needs, or undo/redo clears what it does not restore. Derived mode that affects the next op is in the hist record; undo/redo restores it.

## Views

A `char[:]` is `{ptr, len, id}`. Storing it does not take the bytes.

- `from_path` — page store until `destroy()`. No document-wide `char[:]` over the file; bytes are `read_at` (or a named-arena copy). Open does not scan the body.
- `from_buffer` — keeps the caller’s slice. Refuses non-empty untracked (`id == 0`).
- `span` — empty means “not one piece” (or `n == 0`), or store-backed bytes with no stable view. That is a payload; callers use `scratch_span` / `read_at`.
- `scratch_span(from, n, a)` / `analysis_span` — view if contiguous, else a copy on the named arena (`a` last). Empty is `n == 0` / past end; OOM and short `read_at` are errors. Do not size `a` to `RTX_FRAME_SCRATCH` and treat a longer range as leftover.

## Safety

Constructors assume dead. Reopen is `d.destroy(); d.from_path(...)`.

A path that gives up is not success: a non-empty original must produce a root; scan / highlight / reparse do not plant markup or set `hl_done` after a missing span. Lex a window, not the body — do not pass `len` as a highlight bound. The **root section is the path kind** (`CODE` if a grammar matches, else `PROSE`). `===` headers still split. Mixed markup stays `UNKNOWN` until a header or BOF — do not invent a path-default for a file with no grammar. `*` / `` ` `` refresh style runs; `=` or a large delete rescans sections. An edit in a grammar section relexes from a checkpoint (every 4 KiB) at least 2 KiB before it and stops at the first checkpoint past it where the lex state matches the old one — checkpoints past an edit are shifted and stale until that proves them; the old runs from there on stand (`incr_smoke` checks this against a full lex). A section over `RTX_HL_FULL_MAX` does the same inside its window: the window lex (lookback lead-in included) plants **window** checkpoints (every 1 KiB) from its anchor and is recorded as `hl_cov`; an edit inside `[lo, hi]` relexes from the last of them 2 KiB before it (emitting from `lo`, as the window lex did) to where it converges, and a fill inside `hl_cov` lexes nothing. A window checkpoint is that lookback's state, never a catch-up seed; the same lex may resume from it when its window moves on (Interactive, Run working set). No seed, an edit outside, a group edit, or any other lex over it drops `hl_cov`: the fill relexes the window as before — never more than the window plus its lookback. A Markdown fill lexes window + `RTX_MARKUP_LOOKBACK` in one pass. A grammar with `"blocks": "commonmark"` is lexed by the Markdown block pass (docs/md_view.md): its block state rides in the checkpoint (planted at line starts only), a relex converges only outside a paragraph, and a bare window lex starts at an anchor ≤ 256 KiB behind the lookback (an info-string fence or the section floor), classifying blocks only up to the lookback — or, with none, starts unsure and paints fence bodies as text (leftover, never a wrong nest; never a walk to BOF). Link reference definitions (`core/md_refs.ccs`) come from that block pass too — the walk stages the definitions it classified over the range it lexed, a file up to 4 MiB is swept once (classify only, checkpointed every 4 KiB; an edit re-sweeps only to where the state converges) — and edits shift them; a definition that comes or goes relexes only when a label the lex asked about changed (a `seen` filter), never a file scan per frame. A frame whose end reads its begin (backrefs, `\G`) or a `cctext.bol` prefix frame is never a seed or a match. A `===` header starts a fresh lex; a window's first section need not start at one. A window's section scan (file over `RTX_MARKUP_SCAN_MAX`) records every header in it — no per-window cap — and reads a header that `to` cuts whole (the merge replaces the sections starting in `[anchor, to]`); its prose rescan covers the lines its edges cut, up to 4 KiB past them (`scroll-dense` / `scroll-txt` in `incr_smoke` check each window against its sections lexed whole). Lex copies die with the frame, not an `analysis` bump. A lex call owns its regex scratch (`RtxRxScratch`, freed at return); a grammar regex sees one line with its newline, as TextMate hands Oniguruma a line. A short `read_at` mid-document is a fault, not EOF.

Commit only after the new value exists: hist after `tree.replace` (reserve coalesced bytes before, commit after); clip after a successful cut; path + `saved_head` after a prepared rename. After a successful mutate, commit or rollback failure is `RTX_ERR_UNRESTORABLE` and sets `d.broken` — further edits refuse (do not return a retryable kind with tree advanced and hist still reserved). Clipboard allocs into a local, then assigns. Empty source is a real clear. A path that gives up is unchanged or `broken` — never a hole that looks retryable.

## Faces

`@typehooks` / `@typeview` sit next to the types they name. A `@typeview` is the application’s allow-list. An `as:` embed retries UFCS on the inner type when the face grants the name. A face fences callers that take the face, not every `RtxDoc *` in the same file. Field writes and `as:` embeds are not gated unless the parameter is the face. Add the next face when a new function would otherwise take `RtxDoc *` and only need a slice — not a suite of faces because the type is wide.

- `as: tree` on `RtxDoc`, `as: doc` on `RtxBuf` — miss on the outer retries on the embed.
- `RtxDocHighlight` — `read_at`, `scratch_span`, `style_at`, `section_at`, `ensure_hl(RtxHlWin)`. It cannot `len` / `line_*` / `insert` / `type` / `save`.
- `RtxDocLayout` — measure may `len`, `line_count`, `line_start`, `line_guess`, `index_covers`, `read_at`, `scratch_span`, `style_at`, `style_next`, `style_cur` / `style_cur_next` (a forward cursor per walk), `mark_edges`, `section_at`, `ensure_hl(RtxHlWin)`, `fold_covers`, `md_block_at` / `outline` (runs the lex planted). It cannot `line_of` / `insert` / `type` / `save`. `view_after_edit` takes a full `RtxDoc*` because it reparses.

Mark motion and fold walk the runs `ensure_hl` already produced. They do not lex ahead, pump, or keep a file-shaped table. Heading pairs use those runs; brace pairs (`{}` `[]` `()`) match on the caret’s 256KiB analysis page plus at most one neighbor page each side (same grain as `RTX_HL_WIN_MAX`, not the 64KiB store). Paint does not `ensure_hl` that span — skip uses whatever runs the layout window already has. A fold is stored only when both ends are in that window. Layout skips interiors; caret and scroll jump to the fold edge; hex ignores folds. Folds are document state (`RtxDoc.folds`, cap `RTX_FOLD_MAX`), shared by every pane on the doc — per-pane folds are a known non-feature.

Grid, hex, and the markup lens (Rich hints, nested children, injected lex) are paint policies over the same bytes and the same runs — see [docs/md_view.md](docs/md_view.md). Pair / prefix / path mark shapes: [docs/mark_arity.md](docs/mark_arity.md).

A Marp deck is one more view of the same bytes ([docs/slides.md](docs/slides.md)): `marp: true` in the first 4 KiB sets a block-pass flag that rides in the checkpoints (a top-level thematic break is a slide separator), the deck index is that block pass over a bounded prefix keyed by the edit stamp (slide numbers need every separator before the caret, as line numbers need the line index), and presenting a slide lexes only that slide. The presenter is host-neutral state; a transition asks the host loop for frames only while it plays (`rtx_present_wait_ms`), so a still slide wakes nothing.

Call sites use the doc face (`d.len()`, `b->line_count()`). Peel `.tree` for `write_fd` / page-store internals.

## Edits

The same file in several panes is several cameras on one document. After a mutation, reparse once (the first camera's turn, after every camera planned its patch) and make every matching camera’s vis rows current — each at its own pane's width and rows (`RtxWs_pane_geom`) — a width/top cache is not an edit stamp. Current is either a refill or an exact patch (`RtxLayoutPatch`): the doc keeps one edit span since the last refresh (`RtxDoc.span`, based at `gen0`); rows laid out by `lines()` — or by a seeking camera's `fill_off` over whole lines — at `gen0` relay only the edited lines plus lines whose runs, reveal, section or MD table widths moved (runs diffed over the window, planned before the reparse), and shift the rest by bytes and lines. A patched seeking camera labels its rows as its refill would (`line_guess(seek_off)`, then the keyed origin). Anything the patch cannot prove — a byte-capped seek window, grid / hex, folds, `=` / large-delete rescans, an edit above the camera or eating its last newline, a table set / column change, a width change — refills.

The document write is `replace` (byte range in `[0, len]`). `insert` / `erase` on the doc call it. User changes are `replace` on the history stack. Dirty is `hist.head != saved_head`. Offsets, caret, and selection are bytes. Save streams pieces (`write_fd`). Deleted bytes stay in the original/add buffers; hist stores them inline only while they still coalesce (typing/backspace), and otherwise keeps piece descriptors so undo splices the range back. A record coalesces only a pure insert onto a pure insert or a pure erase onto a pure erase; a replace that does both, and every mark / table policy write (join, apply, unwrap, toggle, row / column), is its own record.

### Edit groups

One command is one undo step. A command that writes several sites
(table column insert / delete / move, a Rich join that pulls in a partner
hint, a Markdown Enter or Tab that renumbers ordered siblings, a pair
wrap or a link paste — docs/md_view.md Editing — later rename /
multi-cursor) is one **group**: `RtxDoc_replace_batch(d, edits,
n)` takes sites in pre-edit offsets, sorted and non-overlapping (strictly
increasing `off`, `off + n <= next off`), and refuses anything else, or
more than `RTX_GROUP_MAX` sites, before a byte moves — an honest error,
never a silent partial edit. One effective site is a plain policy
`replace` (its own record); a single `replace` keeps its fast path.

Members sit contiguous on `hist.recs` (still raw + `cc_arena_realloc`)
with one group id, in apply order — descending offset, so each record's
`off` is exact when it lands. Undo / redo move over the whole run; head
never rests inside a group, so dirty (`head != saved_head`) is at group
granularity. Undo restores the caret from the first member, redo from
the last (a policy path that places the caret patches the last member,
as single records do). Typing never coalesces into a group, and the
next keystroke is its own record. Per-site revert would be a separate
command over the members, not undo.

Every group move (batch, undo, redo) is one path, `rtx_group_apply`:
joins find and the island once, writes the tree in descending offset
order, then patches once — one `edit_gen` bump, secs / runs / folds /
tm checkpoints in one pass (equal to the per-replace shift of every
site, which the smoke checks), the edit span noted per site so it is
the union, `last_off` at the lowest site, and a full reparse only when
a site is `=` / a large delete / in another section. Up to 16 sites
patch the find list per site; more wipe it once and the pump rescans.
The line index is patched per site inside the tree (`note_insert` /
`note_erase`). All history needs — owned bytes, piece refs for deletes
above `RTX_HIST_INLINE_MAX`, room on `recs` — exists before the first
write; commit is after. A member that fails rolls back the members
already written (unchanged); a member or rollback that cannot be put
back latches `broken`. Undo ops may share an offset when the lower one
erases nothing (a pure delete touching the next site). A 10k-site rename
on a highlighted file with a live find list is ~35 ms as one group vs
~1 s as 10k `replace`s.

**Bulk.** A group of at least `RTX_GROUP_BULK_MIN` (1024) sites, none
deleting more than `RTX_HIST_INLINE_MAX`, is not k tree writes and not k
records. One pass over the sites (`RtxBulk`) builds the post-edit piece
sequence of the touched range `[lo, hi)`: an untouched span of
`RTX_BULK_GAP_REF` (1 KiB) or more stays a ref into the original / add
buffer; a shorter span and every inserted byte go, in order, into ONE
contiguous add append, staged past the published add end
(`RtxPageStore_stage_add`) so find lanes still read the add file. So a
dense group costs O(len / GAP_REF) pieces, not two per site (a piece is a
~80 B node; 1.3M sites as refs would be 200 MiB of nodes). The build only
reads the tree; dropping it before commit edits and publishes nothing.
Commit is one `RtxPieceTree_bulk_swap`: the whole piece list rebuilt as a
balanced tree, O(pieces), contiguous neighbours merged (undo folds back
into the original piece). Exact line weights stay exact (suffix rebuilt
from `lo`); progressive ones reset the prefix to `lo` and the pump
rebuilds honestly; pins at or after `lo`, the origin and the island
drop. Analysis sees one replace of `[lo, hi)`: span, secs / runs / folds
/ checkpoints shifted as one edit, find wiped, `last_markup` = a full
reparse. The record (`RtxHistBulk`, session) keeps the old and the new
ref sequence — undo / redo are swaps, deleted bytes stay in the buffers —
and the site list once (`enc`: varint gap / del / ins, payload bytes
deduped against the previous site), which is the journal payload.
Replace-all streams its sites into the build as the scan finds them
(below). `rtx_bulk_tune` / `-D` move the thresholds for tests.

The Safe journal writes each record's group id, transaction id and an
END flag on the group's last member (`RTXS` v7). Load keeps a group only
with its END: an unterminated trailing group — the leaf a writer that
died mid-group would leave — is dropped whole and head falls back to
its edge; an interrupted group, or a head / saved inside one, ignores
the journal. v8 adds the bulk record: one record (flag 2, END set) whose
lengths are its range's and whose payload is `enc`; load checks that it
decodes to them, and the record carries no sequences until its first
undo / redo rebuilds them from `enc` (forward for redo, inverse for an
undo past a save). v1–7 still load (v1–6: every record its own).

A group that spans documents (a workbook rename that also edits a CSV
header) is a workspace transaction: `RtxWs_replace_xact` applies each
member doc's group stamped with one transaction id (all or nothing — a
failing member undoes the others and drops that redo). Undo in any
member undoes all while every member is still at head
(`RtxWs_undo_scope` = ALL); if one moved on, undid it, or is parked it
asks (all files / this file / cancel — `ask_xact` in shared UI, the TUI
status bar, a native dialog in cctext-ui). "All" undoes every member back
through the transaction, their later edits too; a parked member refuses
it. Redo redoes all only when every member sits just below. The
membership table is per process: after a restart the journaled ids
remain on the records but each doc undoes on its own.

Save defaults to a **safe rename** (sibling `path.tmp.XXXXXX`, mode bits,
fsync, `rename`, best-effort dir fsync). That replaces the inode: hard-link
identity is lost, and a symlink at `path` is replaced rather than followed.
Owner, ACL, and xattr are not copied. `--backup` is the other policy: write
through `path` (follows the symlink / keeps the inode). When the dest is the
opened original and its size still matches, only the dirty span is copied
and overwritten — unchanged prefix (and a same-length original suffix) stay
on disk. A length-changing write that still streams original pieces pins
those file bytes before the first `pwrite` (the dest inode is the page
store’s original). A tail above `RTX_SAVE_PIN_MAX` writes a full temp and
copies onto the path. `path~` is then `RTXB` (magic / ver / old_len / lo)
plus the old bytes that were about to be overwritten. A pure append writes
no `path~`.
If the dest is another path or the size drifted, fall back to a full copy
then truncate+write. Crash mid-write can leave a mixed target; `path~` is
the recovery. Save stamps mtime + size + inode at open and after a
successful write. A later save of that same path refuses if the identity
drifted (`file changed on disk`) unless the user overwrites — then the
write is whole-file, not a `--backup` dirty span. A missing dest is not
a conflict (recreate). `--batch` save fails with that error; it does not
overwrite. Safe journals use the same triple (mtime is nanoseconds). A
clean journal tosses hist on mismatch. A dirty one is never tossed: its
flush pins the base (a copy of the bytes hist is relative to), and a
drifted file reopens from that pin, dirty, stamped with the old identity
so Save asks. No pin: the journal is held untouched, the buffer shows
the file, counts as unsaved, and save-all refuses.

### Replace

Replace is a find command that writes, and it is ordinary edits. Replace
one works on the pinned hit when it is exactly the selection. It makes one
`replace` (one record) and then selects the first hit at or after the
replacement's end, wrapping like next. If no hit is selected, the first
press only selects one. A small document (up to one find block) finishes
its list at once so the next hit is there to select; a larger one leaves
it to the pump.

Replace all is a job (`RtxReplace`, owned by the caller: `RtxUi.rjob`, a
test, the batch verb). It compiles its own copy of the query and makes its
own leftmost non-overlapping pass. It does not use the find list, because
literal hits overlap, zero-width matches are skipped, and the list stops at
`RTX_FIND_MAX`. Each step reads a window of `RTX_REPL_SLICE` starts, plus
`RTX_FIND_CTX` before it and the program's longest match (or
`RTX_FIND_SPAN`) after it. It stages every site: the replacement is
expanded from a re-match (`rtx_rx_at`) when the template names a capture,
and a site whose replacement equals its bytes is skipped. Nothing moves
until commit. Below the bulk threshold the job keeps a site table and
commit is one `rtx_doc_replace_group`; from it on, the table stops and
each site is fed to a streaming bulk build in the same pumped steps (the
header keeps repainting, Esc still drops it), and commit closes the range
and swaps. Either way: one undo step, one Safe group with its END, and
the caret and anchor at kick mapped through the sites as they came. A
cancel is a drop, so there is never a partial edit. 1.3M sites in
100 MiB: scan + build 0.3 s in 48 ms turns, commit 4 ms, +14 MiB peak,
undo / redo under 1 ms, a 4 MiB journal. A job that
sees `edit_gen` move since its kick refuses. So do a match longer than
`RTX_FIND_SPAN` (checked wherever the window edge falls), more than
`RTX_GROUP_MAX` sites, or more than `RTX_REPL_BYTES_MAX` of staged ins+del
bytes. Each refusal is a static message with the offset, never a clipped
result.

Zero-width matches: after an empty match the search steps one scalar. An
empty match right after a non-empty one counts (Python `re.sub`). With the
whole document as the range, EOF is a position; with a range `[lo, hi)`
(a multi-line selection, unless Alt-L), a site must end by `hi` and `hi`
itself is not a position. The template is `$0`-`$9`, `${N}`, `${name}`
(names are kept on the program, `rtx_rx_group_named`), `$$`, `\n`, `\t`
and `\\`, parsed once per job. A group the pattern lacks, or one above 9,
fails the kick. Without REGEX the replacement is verbatim.

`--batch` is a headless command host (no TTY): `-c` lines or a stdin script.
Verbs are semantic (`goto @N` / `N%` / `LN`, `await index|island`, `print`,
`read`, `insert`, `undo`, `redo`, `replace`, `save`, `stats-json`, `quit`) —
not keystrokes. `replace [--all] [--regex] [--icase] [--word] [--] FIND
REPL`. An argument is bare bytes up to white space, `'…'` (verbatim), or
`"…"` where only `\"` is an escape, so regex and template backslashes pass
through; `""` is an empty replacement. Without `--all` it replaces the
first match at or after the caret. Progressive
work must be awaited; `goto` does not kick an island. Safe journals are off
unless `--safe`.

Browse-away does not ask and does not write the user’s path. It flushes the journal (hist as bytes, camera, identity) and **evicts** the document epoch only when that hist write landed or the buffer is clean. A dirty journal miss keeps the epoch live and is a visible fault. An idle flush miss must not mark hist current — otherwise a later park would evict on a stale “already flushed” note. Live set is the buffers the live panes show (any number of panes). Untitled cannot park. Quit still asks; `q` drops dirty journals. A later suspend quit is not this cut.

## Encoding

Offsets, caret, selection, and the piece tree are bytes. Text views walk
**extended grapheme clusters** (UAX #29 GB3–GB13: combining marks, ZWJ emoji,
regional-indicator flags, Hangul, Indic conjuncts) for motion, wrap, hit-test,
backspace, and measure. Hex is a paint of that same editor, not a second one: caret, selection,
and unlock stay on the camera / document. Grid is the same: one record
per newline whose window-lex face is not STRING (the grammar's begin/end,
via lookback — not a CSV quote walker). No lex: every NL is a record.
`rtx_layout_grid_held` is that STRING test; a delim or NL is a gap only
when it is not held. Line 0 is a sticky header (always `read_at(0)`, not
`line_start` from a gap). Widths are header ∪ this fill (max line in a
cell). Column count follows the record. Each column caps at
`RTX_GRID_MAX_COL`, not the pane; extra fields use `left_col`. A cell
wraps on its width and on embedded newlines. A
delim inside a string is not a field. The line prefix is still newlines.
Three lengths, not one: `RTX_FRAME_SCRATCH` is a stack copy, not a record;
`RTX_MARKUP_LOOKBACK` is the mid-field `cam_lo` walk; `RTX_GRID_REC_CAP`
is a record leftover. A miss is leftover, not a record index. Wheel/scroll
steps whole records (not vis wraps). Each wrap row keeps its physical
line in the gutter. Vis-row Home/End is `rtx_layout_soft_wrap` (wrap view
or grid). `L.wrap` is only the wrap view — grid sets it to 0. Horizontal
camera is `rtx_layout_uses_left` (default and grid). `l` cycles default /
wrap / hex / grid. A split is two editors (same
file or another path); focus is which pane. Hex motion already steps one
byte (and resets to the high nibble). On a hex pane, `type` accepts only
`0-9a-fA-F` and overwrites that nibble. The dump is display (UTF-8 lead,
continuation ·, else .), not a caret of its own. Leaving hex with a
collapsed caret snaps it to a cluster start. `read_at` stays bytes.
Invalid bytes are one-byte clusters (U+FFFD, width 1). Cluster width is the
cluster, not only the first scalar: ZWJ emoji, RI flags, and VS16 emoji
presentation are 2 columns; extend/ZWJ glue adds none; otherwise the first
scalar’s East-Asian / `rtx_utf8_cp_width` policy (then `cols >= 1`).

**Marks are clusters with one more join rule.** A markup span (`**bold**`,
`` `code` ``, a fence) is hint bytes around **one** content. In a
Rich pane the hints are atoms exactly the way a ZWJ sequence is: never an
interior caret position, one step to cross, painted at zero width. The
extra rule is that a pair’s two hints are **one atom in two places** —
what removes one removes both (backspace on a hint is unwrap), what
selects one selects both (a cut that reaches a hint reaches the pair).
Content between the hints is ordinary clusters, so interior positions are
legal; that is the only way a mark differs from a glyph. There is no
“extend the selection” policy and no broken-markup case — those are the
join rule. Prefix marks (`# `) and path marks (`[label](dest)`) **extend**
that sentence — they do not use this join; see
[docs/mark_arity.md](docs/mark_arity.md). Implement the pair rule where
clusters already are: an atom-length beside `rtx_utf8_cluster`, consumed
by motion, selection, delete, wrap, and hit; gated on a per-fill
`has_marks` so a plain file never pays. The write path is
`RtxDoc_replace_join` (one `replace`, or one edit group when it pulls in
the partner hint); Rich panes call it via
`rtx_buf_pair_replace`, Source mode (`layout.rich == 0`) has no hint
atoms — hints are plain bytes. See
[docs/md_view.md](docs/md_view.md).

**Discover vs transform.** Nav (`Ctrl-K/P`) steps among marks that already
exist in the window (`hint_a > 0`). Apply (`.` / `h` / `1–9`) plants or
cycles from the grammar’s `apply` catalog. Nav does not invent a mark when
none is under the caret; that is apply’s job. Invalid (`Ctrl-E/R`) is a
separate axis (scope prefix `invalid`). Do not hard-code a keyword list
as “marks.”

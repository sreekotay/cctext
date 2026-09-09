# Markup lens: rich documents over bytes

Markdown is the **first client**, not the product. The product is a lens
that any grammar can drive: true styling from grammar metadata, hint bytes
that a Rich pane paints at zero width, marks that move like clusters, a
toolbar generated from `apply` tags, and — on the block side — nested
child views (tables, fences, opaque renders) and folds.

Source of truth stays **document bytes**. Nothing here is a second
document, a Markdown→HTML store, or a WYSIWYG dual. Every feature is a
run, a layout policy over runs, or a fold. Product shape that must still
hold: DESIGN Interactive (seeking / lined camera, cover, gutter key, pump,
window) and DESIGN Encoding (byte caret, window highlight, grid as a view
over bytes, **marks are clusters with one more join rule**).

## Status

Landed (the "fix first" batch, 2026‑09‑05):

| Piece | Where | State |
|---|---|---|
| Runs carry a **planter** (`RTX_RUN_PROSE` / `RTX_RUN_TM`); window re‑lex clips by planter, not bit shape | `RtxRun.planter`, `rtx_run_planted_*` in `core/document.ccs` | done — fixes the mixed‑run leak |
| `RTX_SEC_MARKUP`: lexed like CODE, `style_at` keeps scope, GUI picks the prose face | `rtx_sec_lexed`, `rtx_doc_path_kind` | done; Markdown opts in |
| Prose scanner: `**x**` bold, `*x*` italic, `` `x` `` mono; runs only on PROSE / unknown sub-ranges (open, window, edit) | `rtx_doc_scan_prose_runs` | done |
| Grammar sidecar: `"cctext": {"kind":"markup"}` (grammar), `bold` / `italic` / `mono` / `apply` (pattern); default scope→bits map when absent; bits copied to every TM run at plant | `RtxTmRt.markup`, `RtxTmRule.sc_*` / `cap_bits`, `rtx_scope_default_bits` | done; `apply` drives the toolbar |
| `markdown.tmLanguage.json` declares `kind: markup` — `.md` is MARKUP: TM scopes + bold / italic / mono bits, prose face in the GUI | `testdata/grammars/markdown.tmLanguage.json` | done |
| Hint byte counts on runs from literal begin/end | `RtxRun.hint_a` / `hint_b` (0 = unclosed) | done; read by `RtxDoc_hint_at` / `mark_at` / `hint_next` / `has_marks` |
| Italic paints: TUI SGR 3, GUI italic / bold‑italic faces | `frontend/cctext.ccs`, `gui_draw.ccs` | done |
| Unwrapped row width = sum of per‑run measures via `RtxDoc_style_next` | `rtx_layout_row_measure` | done; scope‑only edges coalesce |
| Heading fold at window edge is a leftover, not a fold to the window end | `nav_region` | done |
| Per‑pane `rich` bit on `RtxLayout`; persisted in `RtxSafeCam` (RTXC v2, v1 reads); `d` / `Ctrl-D` / Cmd-D toggles (`RtxBuf_toggle_rich`); status shows `rich` | `layout.cch`, `safe.cch`, `ui_cmd.h` `CMD_RICH` | done |
| Rich layout: `has_marks` per fill; `rtx_layout_hidden_to` skips hidden hint bytes in row measure, wrap walk, `x_of`, hit, and all four text paint loops (TUI active / preview, GUI active / preview); grid and hex ignore `rich` | `core/layout.ccs`, `cctext_draw.ccs`, `gui_draw.ccs` | done |
| Reveal on entry: `rtx_layout_reveal` = union of `mark_at(caret)`, `mark_at(anchor)`; `ensure_view` refills when the span changes; `move_vert` scratch rows inherit it | `rtx_layout_reveal`, `RtxBuf_ensure_view` | done |
| Join rule: `RtxDoc_replace_join` — a replace touching one hint removes both hints whole, one `replace` over the union span, one hist record (cap `RTX_HL_WIN_MAX`, else plain). Pane hooks on Rich only: `move_horiz` never rests inside a hint (`rtx_buf_hint_snap`); backspace / delete on a hint unwraps; selection delete, cut and type‑over go through the join. Copy is the bytes as selected. | `core/document.ccs`, `RtxBuf_backspace` / `delete_forward` / `type_cp`, `RtxWs_cut` | done |
| Inline spans stay on one line: `"cctext": {"inline": true}` drops an unclosed span at EOL (opener is text, CommonMark unmatched delimiter); `"flank": true` rejects an opener before whitespace and a closer after it (`2 * 3` is plain). Bold / italic / code declare `inline`; bold / italic add `flank` | `RtxTmRule.inl` / `flank`, `rtx_tm_span_advance`, `markdown.tmLanguage.json` | done |
| Fence + injection: `cctext.info` + `scopeName`; guest lex at depth 2; `RTX_RUN_INJECT`; nested `#italic` / `#bold` / `#code`; Python `"""` / `'''`; closer always first; `style_at` walk-back does not stop at a sibling | `RtxTmRule.info` / `embed_scope`, `rtx_tm_rt_for_scope`, `rtx_tm_lex`, `rtx_doc_run_first` | done; HTML `<script>`/`<style>` `RE_SPAN` landed |
| Fixtures with line‑numbered expectations | `testdata/rich/md/`, `testdata/rich/code/` | in tree; table classify smoke written |
| MD table child: classify + fill-epoch geom; Rich aligns cells, hides `|`; TUI paints `│` rails and the sep as a `├─┼─┤` rule; GUI is a clipped stroked grid (pixel col widths, no box-drawing); Source stays raw; `x_of` / hit through cells; motion skips the rule; `|` and cell pad are not a caret landing (`rtx_layout_md_snap`); classify is window + `RTX_MARKUP_LOOKBACK` so a header above the fill still keeps body rows as a table; wrap-on fits columns to the pane and wraps cells (GUI and TUI grow row height; paint every wrap line); paint / hit / `x_of` / caret share `rtx_layout_md_cell_wrap` | `rtx_md_table_*`, `RtxLayout.md_*`, TUI/GUI paint | done this cut; no invented top/bottom box |
| TM lowering audit against the embed fixtures | [docs/grammar_audit.md](grammar_audit.md) | written |
| Mark arity (pair / prefix / path): headings and links share the lens, not the pair-join | [docs/mark_arity.md](mark_arity.md) | prefix + path two-run plant + named apply table landed |

Not landed: opaque renders, blocks‑as‑folds,
heredoc / lookaround regex spans. Rich hit‑test can still land
between the two bytes of a *revealed* `**` (the next motion snaps out);
hidden hints are never a landing. `\*` escapes and `_` marks have no rule
yet, so they paint as source.

## Vocabulary

- **Mark** — a grammar span with an **arity** ([mark_arity.md](mark_arity.md)).
  A **pair** is `hint_a` + one content + `hint_b` (`**bold**`, `` `code` ``,
  fence). A **prefix** is `hint_a` on the opener only (`# `, later `> ` /
  `- `); `hint_b = 0` is not a closer. A **path** is two **faces** (label
  and dest): `[text](url)` is not a pair. `hint_b = 0` on an unclosed pair
  at the window end is still a pair — prefix is the sidecar, not that.
- **Hint** — the delimiter bytes. Always in the file; Rich pane paints them
  at zero width (unless revealed). Dest-face **content** is also hidden
  until that face is entered.
- **Content** — ordinary clusters of a face (pair interior, heading title,
  link label). Dest is a second content face.
- **Face** — path only: `label` (shown) or `dest` (hidden until entered).
  Two runs, no face id on `RtxRun`.
- **Atom** — what motion crosses in one step. In Rich a **pair**’s two
  hints are one atom in two places (DESIGN Encoding). Prefix opener is
  one-sided. Path join never crosses a face. Source has no hint atoms.
- **Source / Rich** — `layout.rich` per pane. Source is today's paint.
- **Planter** — who produced a run (prose scanner, host TM lex, guest
  inject). Clip and toggle‑off are by planter, never by bit shape.
- **Section kind** — `PROSE` (no grammar; scanner), `CODE` (lexed; mono),
  `MARKUP` (lexed; prose face). Kind decides lex and font; the run decides
  style.
- **Child** — a layout‑epoch measure / paint / hit over a byte span with a
  local policy (table, fence, opaque). Not a node, not a second doc.

## Zero cost when unused

A plain file, a CSV in grid, a hex dump pay nothing:

- Style bits live on `RtxTmRule` and are copied to the run at plant time
  — one byte compare per plant, no lookup per glyph.
- `L->has_marks` is set per fill when any run in the window has
  `hint_a > 0` and `L->rich`. Every Rich path (skip, atom step, reveal) is
  behind that gate; `rtx_utf8_cluster` is unchanged.
- `style_next` on a document with no runs is one compare.
- Grid and hex ignore `rich` entirely (hidden hints would make column
  widths lie; keep grid the CSV lens).

## Style from grammar

Two sources, sidecar wins:

1. **Sidecar** on the pattern: `"cctext": {"bold":true, "italic":true,
   "mono":true, "apply":"bold"}` — already parsed.
2. **Default scope→style map** beside the theme map in `core/scope.ccs`:
   `markup.bold → bold`, `markup.italic → italic`, `markup.raw → mono`,
   `markup.heading → bold`. Third‑party grammars dropped into
   `testdata/grammars/` never carry a sidecar; this makes them useful.
   Consulted at plant time only.

Plant path: TM match → one run with scope **and** bits **and**
`hint_a/hint_b`, planter `RTX_RUN_TM`. Bits are resolved once per rule at
bind (`rtx_tm_bind_bits`; capture scopes in `rtx_tm_intern_caps`), so a
plant is three bit copies. The prose scanner is the planter only for
`PROSE` (or still‑unknown) sub‑ranges — `rtx_doc_scan_prose_runs` walks the
section list, so a grammared section never carries `RTX_RUN_PROSE` runs.

Section/font: `MARKUP` gets the prose face; `st.mono` (inline code, fence
body) gets mono inside it. The shipped `markdown.tmLanguage.json` declares
`kind: markup`, so `.md` is MARKUP now.

## Hints, Source and Rich

| | Source (`rich == 0`) | Rich (`rich == 1`) |
|---|---|---|
| Hint paint | plain bytes | zero width, **revealed** when caret or selection is inside the mark |
| Hit‑test at a hint x | byte | resolves to content edge (hints are not a landing) — no ambiguity because an adjacent caret reveals them |
| Motion across hints | per cluster | one step crosses the pair edge; interior positions are content clusters |
| Backspace on a **pair** hint | deletes a byte | unwrap: removes both hints of that pair (one atom in two places) |
| Backspace on a **prefix** (`# `) | deletes a byte | **apply** on the opener (demote / unwrap); title type-over does not eat `# ` |
| Backspace on a **path** `[` / `](` | deletes a byte | **apply** unwrap: keep the label, drop `[` `](dest)`. Not pair-join |
| Selection reaching a pair hint | as bytes | reaches that pair; cut removes the pair |
| Selection of a path label | as bytes | **named face** — does not include dest bytes |
| Copy | bytes | bytes, hints included (truth is bytes) |
| Find hit inside a hint | plain | selects the match and reveals the mark |
| Line‑start hints (`# `, `- `, `> `) | plain | zero width; gutter / line numbers unaffected |
| Wrap and `x_of` | as today | skip hint bytes at layout time (same place soft‑wrap already walks clusters) |

"Reveal on entry" (Typora / Obsidian live preview) is what makes the rest
cheap: the caret can never sit on an invisible byte, so no second
coordinate system and no "skip vs stick" policy.

Implementation site: an atom‑length beside `rtx_utf8_cluster` consumed by
motion, selection, delete, wrap and hit — motion post‑steps the way
`fold_snap` does. All gated on `has_marks`.

## Apply and toolbar

- The apply table is a **grammar property**, not a caret property.
  `RtxDoc_apply_list` is unique `cctext.apply` names on the document's
  path grammar (`rtx_tm_apply_list`). Untitled / no path → empty. The
  bar is not filtered by “you are in a heading.” Each name does that
  arity's verb at the caret, or no-ops. Hosts do not hard-code Bold.
- Apply kind (`RTX_APPLY_PREFIX` / `PAIR` / `PATH`) is **derived**:
  arity plus “this rule can transform” (`insert` / `wrap` / lit span).
  Not a sidecar key, not stored on the run.
- Apply = **one** `replace(lo, hi-lo, a + bytes + b)`: one hist record,
  no new primitive. Cap the selection at `RTX_HL_WIN_MAX` and refuse
  above (honest leftover) — the copy is the cost.
- Toggle‑off finds the enclosing run by rule (planter + `RtxRun.rule`
  tag). Include clones of `#bold` share `cctext.apply`, so unwrap
  compares the looked-up name, not tag equality. Pair: same unwrap as
  backspace on a hint. Prefix / path: apply, not `replace_join`.
- Prefix apply: `apply` is the toolbar name, `insert` is the unit
  (`#`, `>`, `- `), `max` is how many stack. Cycle the prefix on this
  line, or insert the first prefix+insert rule. Path unwrap is apply;
  path wrap is `[sel]()` (dest empty; caret in dest). `link` on the
  bar does both.
- Pair apply (bold / italic / code / autolink) uses the same name: wrap
  is `a + bytes + b` from begin/end, or from `wrap` + `insert` bookends
  (`"<>"` + `wrap: 1`). Toggle-off unwraps the covering mark.
- Cmd-. (Ctrl-. / Esc-.) opens the on-screen apply menu; `1–9` picks (cap 16 names,
  nine digits). Cmd-1..9 applies directly. Shift-Cmd-H is prefix cycle
  (`CMD_APPLY` / `KEY_APPLY`), not a heading verb.
- **Nav is not apply.** `Ctrl-K/P` steps runs with `hint_a > 0` already in
  the window (the same mark vocabulary). It does not plant. A code buffer
  with only paint scopes correctly reports “no mark.” Plant / cycle with
  the apply keys above.
- `cctext.bol` is the content-line start: physical BOL, or only
  whitespace since an open prefix opener (stack, not planted runs).
  A quote line can therefore host a list / heading prefix. Fence that
  spans quote lines still needs `while`.
- `- [ ]` ↔ `- [x]` is `apply: toggle` on a 3‑byte box (`[ ]` / `[x]` /
  `[X]`), `insert: "[ ]/[x]"`: one `replace`. Kind is derived from the
  slash in `insert` (not a sidecar kind, not a fourth arity). The box
  is a list inner (`cctext.bol` after the prefix) and must be followed
  by a space (`[x]no` is text). `[X]` toggles off to `[ ]`.
- TUI: Esc-. then `1–9`. GUI: Apply menu / Cmd-1..9. Same table.
- Children (tables, later math / image) are layout-epoch policy, not
  marks. Host GFM classify is leftover. Do not invent `arity: table`.

## Code embeds and injection

This is the same lens; MD fences are only the depth‑0 client. Docstrings
with SQL, JS template literals, `<script>` / `<style>`, shell heredocs, YAML
block scalars, `#if 0` all want *lex this span with that grammar*.

Landed with wedge 4 (2026‑09‑05): the begin/end stack is live (`sp > 1`).
A fence with `"cctext": {"bol": true, "info": true}` records the info
string, resolves it via `rtx_tm_rt_for_info()` (`python` →
`source.python` through `scopeName` / title / `fileTypes`), and guest‑lexes
the body. Guest runs are `RTX_RUN_INJECT` and clip with TM. The current
closer is always tested first, so italic `*` cannot steal bold `**` and a
BOL ``` wins over an unclosed guest span. Same‑grammar nest
(`***both***`, `**a *b* c**`, `*outer **inner** outer*`) works because
bold/italic list each other as inners and `cctext.flank` rejects a closer
after whitespace. Python ships explicit `"""` / `'''` spans.

HTML `<script>` / `<style>` regex spans landed. Still out: lookaround,
heredocs, JS templates, `while`, `injections`, host islands (`${}`, f‑strings).
Detail in [docs/grammar_audit.md](grammar_audit.md).

## Nested children (block side)

A **child** is measure / paint / hit over `[lo, hi)` with a local policy.
Locked shape (unchanged from the first cut):

- **Layout‑epoch scratch**, destroyed with vis‑row reset — the honesty of
  `RtxGridGeom.fields` ("last split, not a second column table").
- **Paint‑time recursion**: parent keeps coarse `RtxVisRow`s (physical
  lines or table records); paint / hit recurse. Flattening cell wrap lines
  into tagged vis rows is rejected (row count and scroll math blow up).
- **One wrap oracle per nested surface**: cell wrap lines are layout‑epoch
  scratch from `rtx_layout_md_cell_wrap` (same honesty as grid fields).
  Row height, paint, `x_of`, hit, selection, and the caret all consume that
  result with the same column width / measure — never a second wrap.
  Body soft‑wrap keeps one vis row per wrap line (that *is* its oracle);
  MD copies the contract, not the storage. Caret height is one cell
  line (`lh`), not the full tall record.
- Caret stays a byte offset; hit recurses; Tab may jump cell `lo` like
  `RtxBuf_move_grid_col`.
- Classification is **window + `RTX_MARKUP_LOOKBACK`** only. A fence opened
  above the lookback or a table whose header is above the lookback is a leftover
  painted as text, never a wrong nest. No progressive MD index; line cover
  (`line_scan_off` / island) stays the only progressive index.

| Child | Bytes | Needs | Not in v1 |
|---|---|---|---|
| Fence | `` ``` `` lines | grammar injection on the body span (see above); mono face; optional inner wrap | fence ⊃ table |
| MD table | run of `\|` lines + optional `\|---\|` row (geom, not a record) | column geom = header ∪ this fill; per‑cell child with inline marks via `style_at`; sticky header as chrome; wheel steps records | widths beyond the fill; auto‑pad `\|`; CSV STRING hold; table ⊃ fence; recursive tables |
| Opaque render (mermaid, math, dot, image) | fence body / `![]()` | one vis row of height H at fill time; content from a **Scan‑table row** (start / step / live / resume / deny) like find / island / browse; cached on layout epoch keyed (span hash, width); GUI only | TUI ASCII art (source or fixed box); blocking the frame; fetch on the layout thread |
| Derived value (formula in a cell) | `=SUM(A1:A3)` literal | paint value in place of content, formula is the hint; layout‑epoch, read‑only, one record in window | editing the value; cross‑window refs; persisted values |

Grid vs MD table: `l` cycles default → wrap → hex → grid and **grid stays
the CSV / TSV / pipe lens**. MD tables appear under default / wrap on a
MARKUP doc, never by binding `|` as a CSV delimiter. Reuse from grid: width
= max over header ∪ fill, per‑cell wrap, `left_col` overflow, sticky header
chrome, record‑step wheel, ephemeral field `lo/hi`.

## Blocks are folds — deferred

Notion‑style toggles, `<details>`, `<div>…</div>`, MD heading regions are
not a block model; they are folds over begin/end runs (DESIGN Faces). Kept
as direction, not a wedge, until:

1. `RTX_FOLD_MAX 8` is either named as a product cap or becomes a Vec.
2. Folds are **document state** (shared across panes) — accept, or move
   fold state to the pane before a Source/Rich split relies on it.
3. Tag pairs lex as spans (HTML tags are `match` today; needs regex
   begin/end with a back‑referenced closer).

Landed already: `nav_region` no longer folds to the window edge when the
next heading is outside the window.

## Host parity

| | TUI | GUI |
|---|---|---|
| Hidden hints, atoms, apply | yes | yes |
| Bold / italic | SGR 1 / 3 | Core Text faces (done) |
| Prose vs mono face | no (one cell grid) | yes, by section kind + `st.mono` |
| Table cells | aligned in columns | proportional, per‑cell child |
| Opaque renders | source or fixed‑height box | rendered child |

Say this out loud in the UI: Rich in a terminal is hidden hints, SGR, and
aligned cells — never the GUI picture.

## Wedge order

Each wedge is zero‑cost when unused and ships behind `@smoke` +
`@perf_check`.

| # | Wedge | State |
|---|---|---|
| 0 | `RTX_SEC_MARKUP`; clip by planter | **done** |
| 1 | `hint_a / hint_b` on runs | **done** (write‑only) |
| 2 | Style bits from sidecar or default scope map, copied at plant; `markdown.tmLanguage.json` declares `kind: markup`; prose scanner gated to PROSE sub‑ranges | **done** |
| 3 | Rich pane: `has_marks` per fill; hint skip in wrap / `x_of` / hit / paint; reveal‑on‑entry; `rich` toggle key; TUI + GUI paint; atom step, unwrap, selection join rule | **done** |
| 4 | Fence + injection: `cctext.bol` + `cctext.info`; `scopeName` / `rtx_tm_rt_for_scope()` / `rtx_tm_rt_for_info()`; depth‑2 guest lex; `RTX_RUN_INJECT`; nested inline marks; HTML `<script>`/`<style>` `RE_SPAN` | **done** |
| 5 | MD table child | **done this cut**: classify, fill-epoch geom, `│` rails + sep rule chrome, aligned Rich paint / hit; Source raw; window + `RTX_MARKUP_LOOKBACK` classify; **paint-time cell wrap** when pane wrap is on (fit columns to pane; taller GUI + TUI rows); wrap-aware `x_of` / hit / caret share `rtx_layout_md_cell_wrap` |
| 6 | Apply / toolbar via one `replace`; toggle‑off by rule id; prefix apply from `insert`/`max` | prefix + pair named apply + grammar table + rule id / toggle‑off **landed** |
| 6b | Path faces: two runs (label + dest); dest hide; `replace_join` refuses dest↔label; unwrap keeps the label | two-run plant + dest hide + join refuse + unwrap + wrap **landed** |
| 7 | Blocks as folds — after the three blockers above | |

Smokes come from the fixtures: each README row under `testdata/rich/*` is
an assertion (`style_at`, `hint_a/b`, `section_at`, fold region, child
geometry) once its wedge lands.

## Locks

| Topic | Lock |
|---|---|
| Scope | Rich docs via grammar; MD first client |
| Section kind | `MARKUP` exists; lex / scope / font decided separately |
| Styling | Bits on `RtxTmRule` (sidecar, else default scope map) copied to the run; clip and toggle by planter |
| Hints | Literal begin/end byte counts on the run; layout‑time skip gated on `has_marks`; per‑pane `rich` bit, never OR'd into `view` |
| Atoms | Pair join is DESIGN Encoding (one content, two hints). Prefix / path extend it ([mark_arity.md](mark_arity.md)); dest never in label `hint_b`; no face id on `RtxRun` |
| Nesting | Stack is live (wedge 4). Join stays pair-only; path is two runs |
| Apply | One `replace`, one hist record; cap at `RTX_HL_WIN_MAX`. Table is the path grammar, not the caret. Kind is derived. Link unwrap is apply, not join |
| Nav | `Ctrl-K/P` = discover (`hint_a > 0` in the window). Apply = transform. Do not hard-code keyword scopes as marks; a `.c` file has none |
| Sidecar | One `cctext` object. Recognition (`bol` / `inline` / `flank` / `lit` / `info`) ≠ topology (`arity` / `face` / `wrap`) ≠ transform (`apply` / `insert` / `max`) ≠ paint (`bold` / `italic` / `mono`). A new key answers one of those. |
| Leftover marks | Setext, reference links, images-as-opaque |
| Children | Layout‑epoch scratch, not an arity. Paint‑time recursion; one wrap oracle (`cell_wrap`) for paint / hit / `x_of` / caret; window classify; leftover, never wrong |
| Renders | Opaque vis row of height H; Scan‑table job; epoch cache; GUI only |
| Derived values | Layout‑epoch, read‑only, one record in window |
| Grid / hex | Untouched by `rich` |
| Blocks | Folds are document state; blocks‑as‑folds deferred |

## Non‑goals

Dual document or HTML store; Notion blocks as identity; collaborative OT;
fully hidden source with no Source mode; a new grammar format (additive
`cctext` keys on TM only); rendering arbitrary HTML as a browser widget;
whole‑file MD outline index; TUI diagram rendering.

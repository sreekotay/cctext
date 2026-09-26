# Mark arity: pair / prefix / path

Same **lens** as [md_view.md](md_view.md) (bytes are truth; Rich is a pane
bit; leftover, never a wrong nest). **Not** the same mark shape. DESIGN
Encoding’s join rule is for a pair with one content. Headings and links
are not that.

This note locks arity. Prefix plant and named apply are in. Path is
two runs (label + dest); unwrap, wrap, dest hide, and join-refuse are
in. Fold is a third axis (wedge 7). Do not reopen the three arities;
do not add a fourth.
Toggle is a transform (`apply: toggle`), not an arity. Children
(tables, later math / image) are layout-epoch policy, not marks.

## Locked split (do not reopen)

| Arity | Shape | Join | Unwrap |
|---|---|---|---|
| **Pair** | `hint_a` + one content + `hint_b` | Touch one hint → both hints of **that** pair (`RtxDoc_replace_join`) | Backspace / type-over-on-a-hint: one atom in two places |
| **Prefix** | `hint_a` on the opener only; `hint_b = 0` | Does **not** invent a closer | Backspace on the opener is **apply** (demote / unwrap), one `replace` of those bytes |
| **Path** | Two **faces**, two runs | Join **names a face** (the run). Never unions dest into label | Keep label, drop `[` `](dest)` — **apply**, not “caret was in the link” |

Prefix clients: any `arity: prefix` + `insert` rule (MD: heading, quote,
list). Path clients: inline link, image `![alt](src)` (label = alt), and
reference link `[text][ref]` (dest face `][ref]`). Autolink `<url>` stays
a **pair** (dest == label). Fold is a third axis (wedge 7). Markdown's
block marks (heading, quote, list, task, setext underline) are planted by
the block pass with these same rules ([md_view.md](md_view.md), Markdown
block pass); the arity lock does not move.

## Plants

| Construct | Today | Status |
|---|---|---|
| ATX heading | Prefix run `#{1,6}` + spaces … EOL (block pass, `heading` rule), `arity: prefix`; closing `#`s a second prefix (`heading_close`) | Landed. Title is ordinary / inner-able. `hint_b = 0`. `#hashtag` is text. |
| Setext heading | Title lines: one `heading` run, no hint; underline `===` / `---`: a hidden prefix (`setext` rule, not a heading scope) | Landed (block pass). Level 1 / 2 read from the underline. |
| Quote | Prefix run `>` markers … EOL (`quote`) | Landed. Nested quotes merge into one hint; lazy continuation lines keep the container; a fence inside a quote is a fence. |
| List | Prefix run marker + spaces … EOL on the item's first line (`list`) | Landed. Indent is not in the hint; content indent is the container's (fences / code inside items). `***` / `---` / `* * *` do not plant. |
| Task box | `[ ]` / `[x]` / `[X]` + space right after a list marker (`task`), `apply: toggle`, `insert: "[ ]/[x]"` | Landed. 3-byte mark; `[X]` → `[ ]`. |
| Pair marks | `begin`/`end` + `rtx_tm_add_span` `hint_a`/`hint_b` | Honest. Do not break. |
| `replace_join` | Pair-only: a replace that touches a pair hint removes both hints. Prefix / path refuse partner union. | Landed. Rich panes via `rtx_buf_pair_replace`; Source is plain `replace`. One `replace`, or one edit group (one undo step) when a partner hint sits outside the touched range. |
| Reveal / snap | `RtxDoc_mark_at` = that run; backspace on a pair hint → `replace_join`; prefix opener → apply | Landed. Per-run reveal is why path is two runs, not one run + face id. |
| Inline link | Two spans: label `begin` `[` `end` `](`; dest `begin` `](` `end` `)` | Landed. Label `hint_a='['` `hint_b=0`; dest `hint_a=']('` `hint_b=')'`. Glue: label closer starts dest. Inners on the label. Wrap is `[sel]()`. |

`rtx_tm_add` on `RTX_TM_REGEX` / `LIT_LINE` plants `hint_a=hint_b=0`.
Captures get scope, not hints, and rule tag `RTX_RUN_NO_RULE` (`document.ccs`
regex cap loop). A rule tag indexes the planter's grammar: only a host
`RTX_RUN_TM` run's tag names a path-grammar rule.

## Extend Encoding (do not replace the sentence)

DESIGN Encoding (~307): **“Marks are clusters with one more join rule.”**

That sentence stays the pair rule. Prefix and path **extend** it:

| Arity | Extra rule |
|---|---|
| Pair | Unchanged: two hints are one atom in two places. |
| Prefix | The opener is a one-sided hint. Content (title / body) is ordinary clusters. Join does not synthesize `hint_b`. Structural change is apply. |
| Path | Two contents. Label is the shown face. Dest is a second face: hidden until entered, then typed as text. Join never crosses a face. Unwrap is apply of the path, not pair-join. |

Pair marks keep today’s hook. A `.c` file still has no marks — paint
scopes (keywords) are not marks; `Ctrl-K` correctly no-ops there. Plant
with apply.

## Discover vs transform

| Axis | Keys | Predicate | Missing under caret |
|---|---|---|---|
| **Discover** | `Ctrl-K/P` | Window run with `hint_a > 0` | Status “no mark” — do not invent |
| **Invalid** | `Ctrl-E/R` | Scope prefix `invalid` | “no invalid” |
| **Transform** | `.` / `h` / `1–9` | Grammar `cctext.apply` catalog | Plant / cycle (`apply_set`) |

One vocabulary from the grammar (hints + arity + apply). Nav walks it;
apply mutates it. Do not hard-code a keyword/heading scope list as marks.

## Sidecar (no new `RtxRun` field)

Prefer `cctext` on the TM pattern. Copy onto `RtxTmRule` like `apply` /
`inline`. Consume via `RtxRun.rule` (`rtx_tm_rule_tag`; toggle-off).
Do **not** add a face id or `apply` name to `RtxRun`. One JSON object;
four questions. A new key answers one of them.

| Layer | Keys | Question |
|---|---|---|
| Recognition | `bol`, `inline`, `flank`, `exact`, `abort`, `lit`, `info`, `block` | Did this match happen, and with which guest? |
| Topology | `arity`, `face`, `wrap` | How many parts, and which join? |
| Transform | `apply`, `insert`, `max` | What is the named unit algebra? |
| Paint | `bold`, `italic`, `mono`, `strike` (else scope map) | How does Rich paint? |

`apply` kind is derived (arity + insert/wrap/lit, or `off/on` in
`insert`). Do not store it. `insert` is a prefix unit, pair bookends
(`<>` + `wrap`), and toggle states (`[ ]/[x]`). Split only if a fourth
client appears. Do not put `apply` on the run.

| Key | Values | Plant |
|---|---|---|
| `arity` | `pair` (default) / `prefix` / `path` | Rule only. Default keeps today’s join. |
| `face` | `label` (default) / `dest` | Path dest only. Layout: if dest and the dest run is not in `reveal`, skip the **whole run** (hints **and** dest bytes). |
| `lit` | `true` | Force `LIT_SPAN` when begin/end are regex metacharacters. |
| `wrap` | `1`–`255` (or `true` = 1) | **Match** plants `hint_a = hint_b = N`. Spans use begin/end lengths. |
| `apply` | toolbar name (`heading`, `bold`, …) | Does **not** plant. Grammar-local vocabulary. |
| `insert` | literal unit (`#`, `>`, `- `), wrap bookends (`<>`), or `off/on` (`[ ]/[x]`) | Prefix apply / Backspace demote, pair wrap with `wrap`, or toggle. Not the begin regex. |
| `max` | 1–255 (0 → 1) | How many `insert` units stack. |
| `flank` | `true` / `"under"` / `"math"` | CommonMark delimiter runs: `*` rules, `_` rules (no intraword), `$` (not `$ 5`, not `$5 and $10`). An empty span never closes. |
| `exact` | `true` | begin / end are a whole run of their byte (`` ` `` never inside ```` `` ````). |
| `abort` | one byte (`"]"`) | That byte, when it is not the closer or an inner, drops the span (a Markdown label: rewound). |
| `block` | `fence` `heading` `heading_close` `setext` `quote` `list` `task` `hr` `code` `html` `math` `front` `ref` `ref_dest` `foot` | The Markdown block pass plants this role with the rule (a pattern may be empty); the TM walk never tries it. |
| `strike` | `true` | Paint bit (`markup.strikethrough` by default). |

`hint_b==0` is **not** arity. An unclosed pair at the window end is still
a pair (`hint_b=0` today). Prefix vs that leftover is the sidecar.

Forced field: none. Dest-hide is a rule bit, checked once per dest run
in the wrap walk (already run-shaped), gated on `has_marks`.

## Grammar

No Onig. No lookaround. No `while`. Additive `cctext` only.

### ATX heading — prefix span

Reject the line `match`. Title must stay ordinary / inner-able, and must
keep `markup.heading` (default bold).

| | Lock |
|---|---|
| Shape | `RE_SPAN` (or literal) **prefix span**: `begin` `#{1,6}[ \\t]*`, `cctext.bol`, close at EOL |
| Hints | `hint_a` = matched opener (`#` + spaces). `hint_b = 0` even though the line ends — the newline is not a closer hint |
| Inners | `#bold` `#italic` `#code` `#link` (fixture L29; `# Title` with `` `code` ``) |
| Sidecar | `arity: prefix`, `apply: heading`, `insert: #`, `max: 6` |
| Empty `#` | Opener may be `#` with no space (`headings.md` L33) |
| Trailing ` ##` | **Second prefix** on the same line (`\\s+#+$`), `hint_a` only. Not a pair with the leading hashes (title type-over must not join them) |
| Type-over | Title clusters; must not eat `# ` unless the selection actually covers the opener |

**Setext** (`===` / `---`): the Markdown block pass decides it at the
underline (it carries the paragraph start as a distance) and plants the
title as a `heading` run with no hint and the underline as a hidden
prefix of a non-heading scope. `---` under no paragraph is a thematic
break. Folds and the outline read the level from the underline.

### Inline link — path, two spans

Reject `\\[[^\\]]+\\]\\([^)]+\\)`. Delimiters are `[` / `](` / `)`.
Label is inners. Dest is a face, not `[^]]+`.

| Run | Bytes | Hints | Sidecar |
|---|---|---|---|
| Label | `[` + label | `hint_a='['`, `hint_b=0` | `arity: path`, `face: label`, `inline: true`, `apply: link` |
| Dest | `](` + dest + `)` | `hint_a=']('`, `hint_b=')'` | `arity: path`, `face: dest`, `inline: true` |

Glue: label `end` is `](`. That opener **starts dest**; it is **not**
label `hint_b`. Dest bytes are dest **content**. Do **not** put dest in
the label’s `hint_b`.

Inners on the label: `#code` `#bold` `#italic` — `` [`redis_async_sketch.ccs`](../redis/…) ``
is a pair mark inside the label face.

Titled dest `[text](url "title")`: title bytes are dest-face content
(fixture: the whole `](url "…")` hidden until dest is entered).

| Leftover | Why |
|---|---|
| Shortcut `[ref]` | Needs the document's definitions (no index); `[text][ref]` / `[ref][]` are path marks |
| Image render | `![alt](src)` is a path mark (label = alt); the picture is an opaque child |
| Bare URL | Link scope, not a mark (`links_images.md` L11) |
| `[text] (` space | Not a link (L13) |

Label end is `\\](?=[(\\[])` and a `]` with neither after it aborts the
label (`cctext.abort`, rewound): `[1] and [docs](url)` links `docs`.
Brackets / parens balance (`[a [b] c](u(v))`).

Autolink `<https://…>`: **pair** via `match` + `wrap: 1` (`hint_a` /
`hint_b` = 1) and `insert: "<>"` so apply wrap/unwrap is grammar-driven.
Dest == label. Today’s join is correct. Any grammar can do the same
(`[[page]]` is `wrap: 2` + `insert: "[[]]"`).

## Runs: two faces, not a face id

**Pick: two runs. No face id on `RtxRun`.**

`replace_join` stays “one pair” **only for `arity: pair`**. A path dest
is pair-**shaped** (`](` + dest + `)`) so reveal / skip / `hint_at` stay
run-local, but dest is **not** a pair mark: join must refuse the dest
partner (unwrap of the link is apply). One mega-run with a face id would
force `mark_at` / reveal / join to take a face; two runs reuse today’s
per-run reveal (`rtx_layout_reveal` = union of `mark_at(caret|anchor)`).

| | Label run | Dest run |
|---|---|---|
| Shown (caret in label) | `[` hidden; label typed | Whole run skipped (zero width) |
| Shown (caret in dest) | `[` revealed with label | `](` dest `)` typed as text |
| Adjacent | Dest starts where label ends (after inners) | |

Inner pair inside the label (`` `file` ``) is a third run, ordinary
pair join, unchanged.

## Motion / reveal / unwrap / apply

| | Pair `` `x` `` / `**x**` | Prefix `# Title` | Path `` [`file`](dest) `` |
|---|---|---|---|
| Atom | Hint pair is one atom in two places | Opener `#… ` is one atom; title is clusters | `[` is a prefix atom; dest hints are dest-face atoms; `` `file` `` is an inner pair |
| Reveal | That mark | Heading span (opener + title). Inners reveal themselves | **Face:** caret in label reveals label only (dest stays hidden). Caret in dest reveals dest. Caret in `` `file` `` reveals the code pair; dest stays hidden |
| Motion | Snap off hidden hints | Snap off `# `; title / inner code as usual | Snap off `[` ; dest clusters exist only when dest is revealed |
| Type-over title / label | Content; selection that nicks a hint joins the pair | Title must **not** eat `# ` | Label (and inner `` `file` ``) must **not** replace dest |
| Type-over dest | — | — | Dest content only; must **not** eat `[` or the label |
| Backspace on opener / `[` / `]` | Unwrap pair (`replace_join`) | **Apply** heading: demote (`###`→`##`) or unwrap to paragraph; one `replace` of the prefix bytes | `[` or `](` : **apply** link unwrap (keep label bytes, including inner marks; drop `[` `](dest)`). Not “caret was in the link” |
| Cut | Selection + pair join | Title cut; prefix only if selected | **Named face.** Visible-label selection does not include dest bytes |
| Copy | Bytes (hints included) | Bytes | Bytes (truth is bytes); a dest-hidden selection copies label bytes, not the URL |
| Apply wrap | `a + bytes + b` | Insert / change the prefix (`# ` / `## `) | Wrap label: insert `[` + `](dest)` + `)` ; dest from toolbar / default |
| Fold | — | Third axis (wedge 7). Not unwrap | — |
| **Nav (`Ctrl-K/P`)** | Next / prev run with `hint_a > 0` in the window | Same | Same (label and dest runs both have hints) |

Nav does not plant. Apply wrap / unwrap is the transform axis above.

## `replace_join` must refuse (path)

Ship this **before** dest paint. Otherwise dest-as-pair is a footgun.

`replace_join` unions partner hints only when the touched run’s rule is
`arity: pair` (today’s default).

| Touch | Must not |
|---|---|
| Label content / inner `` `file` `` | Add dest hints or dest bytes |
| Label `[` | Add dest `](` / `)` or dest content. Do not treat `[` + dest as one pair |
| Dest `]` / `](` / `)` | Add label `[`. Do not delete dest **content** as a “hint”. Do not pair-join dest delimiters (that would unwrap dest and leave `[label url`) |
| Selection spanning both faces | Extra union. Plain `replace` of the selected bytes only (leftover, honest) |
| Prefix `# ` | Partner closer (none). Backspace / type-over **on the opener** is apply, not this join |
| Any path run | Store dest in label `hint_b` |

Cap `RTX_HL_WIN_MAX` unchanged. Source panes never call this.

## Wedges

Do **not** steal wedge 5 (MD table child: classify is in tree; paint /
hit next).

| # | Slice | State |
|---|---|---|
| 5 | MD table child | classify + paint / hit landed. Lookback / cell wrap leftover. Not an arity. |
| **6** | Apply / toolbar | Prefix + pair named apply + rule id / toggle‑off + `apply: toggle` **landed**. |
| **6b** | Path faces | Two-run plant, dest hide, join refuse, unwrap, wrap `[sel]()` **landed**. |
| 7 | Blocks as folds | Unchanged. Setext / heading regions wait here (or a later prefix client), not 6b |

## Zero cost

Unchanged gate: `L->has_marks` per fill when `rich` and any `hint_a`.
Prefix / path plant only on MARKUP rules that set the sidecar. Dest-skip
is behind that gate. A `.c` file has no hinted runs — one compare, no
arity / face / dest walk.

## Must not

| Out | Why |
|---|---|
| Lookaround | Setext / “title is heading because next line is `===`” (the block pass decides at the underline) |
| `while` | Quote / list continuation (the block pass carries containers) |
| Onig | Engine lock |
| Dest in label `hint_b` | Join would delete the URL as a hint |
| Face id on `RtxRun` | Two runs + rule sidecar are cheaper |
| Shortcut reference links | No definition index |
| Images as opaque | The render is a child / later; `![]()` is a path mark |
| Steal wedge 5 | Table paint / hit is already queued |
| Second document / HTML store | Lens lock |
| Infer prefix from `hint_b==0` | Collides with unclosed pairs |
| Caret-filtered apply bar | Table is the path grammar |
| Keyword / heading scope list as “marks” | Nav is `hint_a > 0`; apply plants |
| Fourth arity / `arity: table` | Toggle is a transform; children are layout |
| Face id or `apply` name on `RtxRun` | Topology on `mark`; verb on the rule |

## Fixtures (expected after 6 / 6b)

From `testdata/rich/md/README.md`:

- `headings.md` L1–6: each `#… ` prefix is one hidden atom; L29: inner
  mono + bold; L33: empty heading; trailing ` ##` hidden (second prefix).
  Setext L14–15 / L19–20: heading runs, levels 1 / 2, hidden underline.
- `links_images.md` L3: label `text` shown, dest hidden; L5: autolink
  pair; L11–13: not links; L14: empty dest / empty label still path
  plants. `` [`file`](path) ``: inner code pair on the label; dest hidden.
  L6 `[text][ref]` / L7–9 images: path marks; L10 footnote: scope.
- Table cell link (`tables.md` L9): same path shape inside a cell; not a
  new arity.

## Fold into `md_view` / DESIGN

Parent lifts; do not rewrite those files here.

- Vocabulary: add **arity** (pair / prefix / path) and **face** (label /
  dest). Narrow **Mark** so `[text](url)` is path, not a pair. A `match`
  with `hint_b=0` is prefix only when `arity: prefix`.
- Hints / Source / Rich table: prefix row (backspace = apply); path row
  (reveal is per face; dest content hidden until entered).
- Apply: named apply from the **path grammar** (`RtxDoc_apply_list`);
  not caret-filtered. Path wrap is `[sel]()` on the two-run plant.
- Encoding pointer: pair sentence stays; one line that prefix / path
  extend it — this file.
- Wedge table: 6 first client heading; **6b** path faces; 5 and 7
  untouched.
- Locks: join is pair-only; dest never in label `hint_b`; leftover
  setext / reference / images.

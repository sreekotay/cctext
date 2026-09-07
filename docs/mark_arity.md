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
list). Path clients:
inline link, later image. Autolink `<url>` stays a **pair** (dest ==
label). Fold is a third axis (wedge 7). Reference links `[text][ref]`
are leftover for v1.

## Plants

| Construct | Today | Status |
|---|---|---|
| ATX heading | Prefix span `#{1,6}[ \\t]*` … `\\n`, `arity: prefix`; trailing `[ \\t]+#+$` is a second prefix | Landed. Title is ordinary / inner-able. `hint_b = 0`. Setext leftover. |
| Quote | Prefix span `[ \\t]{0,3}>([ \\t]*>)*[ \\t]*` … `\\n` | Landed. `bol` is virtual after an open prefix. List / heading are quote inners. Fence across quote lines leftover (`while`). |
| List | Prefix spans `[-*+]` / `[0-9]{1,9}[.]` / `[0-9]{1,9}[)]` … `\\n` | Landed. Indent is not in the hint. `***` / `---` do not plant. Task boxes leftover. |
| Pair marks | `begin`/`end` + `rtx_tm_add_span` `hint_a`/`hint_b` | Honest. Do not break. |
| `replace_join` | Pair-only: a replace that touches a pair hint removes both hints. Prefix / path refuse partner union. | Landed. Rich-only; Source is plain `replace`. |
| Reveal / snap | `RtxDoc_mark_at` = that run; backspace on a pair hint → `replace_join`; prefix opener → apply | Landed. Per-run reveal is why path is two runs, not one run + face id. |
| Inline link | Two spans: label `begin` `[` `end` `](`; dest `begin` `](` `end` `)` | Landed. Label `hint_a='['` `hint_b=0`; dest `hint_a=']('` `hint_b=')'`. Glue: label closer starts dest. Inners on the label. Wrap is `[sel]()`. |

`rtx_tm_add` on `RTX_TM_REGEX` / `LIT_LINE` plants `hint_a=hint_b=0`.
Captures get scope, not hints (`document.ccs` regex cap loop).

## Extend Encoding (do not replace the sentence)

DESIGN Encoding (~307): **“Marks are clusters with one more join rule.”**

That sentence stays the pair rule. Prefix and path **extend** it:

| Arity | Extra rule |
|---|---|
| Pair | Unchanged: two hints are one atom in two places. |
| Prefix | The opener is a one-sided hint. Content (title / body) is ordinary clusters. Join does not synthesize `hint_b`. Structural change is apply. |
| Path | Two contents. Label is the shown face. Dest is a second face: hidden until entered, then typed as text. Join never crosses a face. Unwrap is apply of the path, not pair-join. |

Pair marks keep today’s hook. A `.c` file still has no marks.

## Sidecar (no new `RtxRun` field)

Prefer `cctext` on the TM pattern. Copy onto `RtxTmRule` like `apply` /
`inline`. Consume via `RtxRun.rule` (`rtx_tm_rule_tag`; toggle-off).
Do **not** add a face id or `apply` name to `RtxRun`. One JSON object;
four questions. A new key answers one of them.

| Layer | Keys | Question |
|---|---|---|
| Recognition | `bol`, `inline`, `flank`, `lit`, `info` | Did this match happen, and with which guest? |
| Topology | `arity`, `face`, `wrap` | How many parts, and which join? |
| Transform | `apply`, `insert`, `max` | What is the named unit algebra? |
| Paint | `bold`, `italic`, `mono` (else scope map) | How does Rich paint? |

`apply` kind is derived (arity + insert/wrap/lit). Do not store it.
`insert` is a prefix unit today and also pair bookends (`<>` + `wrap`);
split only if a third client appears. Do not put `apply` on the run.

| Key | Values | Plant |
|---|---|---|
| `arity` | `pair` (default) / `prefix` / `path` | Rule only. Default keeps today’s join. |
| `face` | `label` (default) / `dest` | Path dest only. Layout: if dest and the dest run is not in `reveal`, skip the **whole run** (hints **and** dest bytes). |
| `lit` | `true` | Force `LIT_SPAN` when begin/end are regex metacharacters. |
| `wrap` | `1`–`255` (or `true` = 1) | **Match** plants `hint_a = hint_b = N`. Spans use begin/end lengths. |
| `apply` | toolbar name (`heading`, `bold`, …) | Does **not** plant. Grammar-local vocabulary. |
| `insert` | literal unit (`#`, `>`, `- `) or wrap bookends (`<>`) | Prefix apply / Backspace demote, or pair wrap with `wrap`. Not the begin regex. |
| `max` | 1–255 (0 → 1) | How many `insert` units stack. |

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

**Setext** (`===` / `---`): **leftover this cut.** Title heading-scope
needs the next line (lookaround or a two-line classifier like tables).
`---` is also a thematic break (`blocks.md`). Do not plant a prefix.
`headings.md` L14–15 stay a later smoke (fold / a later prefix client),
not wedge 6b.

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

| Leftover v1 | Why |
|---|---|
| `[text][ref]`, `[ref][]`, `[ref]` | Reference links |
| `![alt](src)` | Opaque child, not a path mark this cut |
| Bare URL | Not a mark (`links_images.md` L11) |
| `[text] (` space | Not a link (L13) |

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
| **6** | Apply / toolbar | Prefix + pair named apply + rule id / toggle‑off **landed**. |
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
| Lookaround | Setext / “title is heading because next line is `===`” |
| `while` | Quote / list continuation |
| Onig | Engine lock |
| Dest in label `hint_b` | Join would delete the URL as a hint |
| Face id on `RtxRun` | Two runs + rule sidecar are cheaper |
| Reference links | v1 leftover |
| Images as path / opaque this cut | Child / later; `![]()` is not 6b |
| Steal wedge 5 | Table paint / hit is already queued |
| Second document / HTML store | Lens lock |
| Infer prefix from `hint_b==0` | Collides with unclosed pairs |
| Caret-filtered apply bar | Table is the path grammar |
| Fourth arity / `arity: table` | Toggle is a transform; children are layout |
| Face id or `apply` name on `RtxRun` | Topology on `mark`; verb on the rule |

## Fixtures (expected after 6 / 6b)

From `testdata/rich/md/README.md`:

- `headings.md` L1–6: each `#… ` prefix is one hidden atom; L29: inner
  mono + bold; L33: empty heading; trailing ` ##` hidden (second prefix).
  Setext L14–15: **not** asserted this cut.
- `links_images.md` L3: label `text` shown, dest hidden; L5: autolink
  pair; L11–13: not links; L14: empty dest / empty label still path
  plants. `` [`file`](path) ``: inner code pair on the label; dest hidden.
  L6–10 reference / image / footnote: leftover.
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

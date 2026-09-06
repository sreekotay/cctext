# Grammar audit: what the lowered TM table can and cannot express

Scope: the ten grammars in `testdata/grammars/*.tmLanguage.json`, the lowering
in `core/document.ccs` (`rtx_tm_lower_rule` → `RtxTmRule`, `core/tm.cch`), and
what "grammar injection at a span" needs on top of them. Fixtures that drive
each requirement live next to this file; see `README.md` for line-level
expectations.

Line numbers are a snapshot: `core/document.ccs` at 4206 lines, `core/tm.cch`
at 121, `core/document.cch` at 342. The TU is under active edit, so re-anchor
with `rg -n 'static int rtx_tm_lower_rule|static int rtx_tm_flatten|static void !>\(CCError\) rtx_tm_lex' core/document.ccs`
before relying on a number. Function names are the stable anchors.

## 0. What the lowering accepts today

The JSON walker (`rtx_jw_pattern_fields`) reads, per
pattern, exactly: `name`, `match`, `begin`, `end`, `include`, `patterns`,
`captures`, `beginCaptures` (same `capn[]` as `captures`), and the `cctext`
sidecar. Every other key falls into
`rtx_jw_skip` and is discarded, so `contentName`,
`endCaptures`, `while`, `applyEndPatternLast`, `injections`, and
`injectionSelector` never reach the table. **Update 2026‑09‑05 (wedge 4):**
`scopeName` and display `name` are stored on `RtxTmRt`;
`rtx_tm_rt_for_scope()` / `rtx_tm_rt_for_info()` resolve them at lex time
(markdown loads before python alphabetically, so embed is not bound at
flatten). `cctext.info` on a `LIT_SPAN` takes the opener line after begin
as the info string. A span `include: source.x` is `embed_scope` on that
rule. `allow_span = 1` for inners and guest top‑level rules, so `sp > 1`.
Guest runs are `RTX_RUN_INJECT`. **Update 2026‑09‑05 (regex spans):**
`begin`/`end` with a regex metachar (`\\`, class, group, `^$`, … — not a
lone `*`, so `**` / `/*` stay `LIT_SPAN`) lower as `RTX_TM_RE_SPAN` and
match through `rtx_re_match` / `rtx_re_match_caps`. A per-frame literal
closer substitutes `\\1`–`\\7` from the begin captures at open
(`stk_close` / `stk_closen`); not stored on `RtxTmCkpt`, so a window
resume mid-heredoc is leftover. `while` and `injections` are still out.
The engine still has no lookaround, no backrefs, no `\\G`, no `(?i:)`.
At grammar level
(`rtx_tm_load_json`, `:2089`) `fileTypes`, `patterns`, `repository`,
`scopeName`, and `name` are read.

`rtx_tm_lower_rule` (`:1327`) classifies a pattern into `RtxTmOp`
(`core/tm.cch:28–36`):

| Input | Op | Notes |
|---|---|---|
| `include` non-empty, no begin/end | `RTX_TM_SKIP` | `#repo` expands in flatten / `rtx_tm_add_inner_resolved`; a span's `include: source.x` is `embed_scope` (wedge 4) |
| `begin` + `end` non-empty, no regex metachar | `RTX_TM_LIT_SPAN` | byte literals via `rtx_tm_starts`; `**` / `*` / ``` / `"""` / `<!--` stay here |
| `begin` + `end` with a regex metachar (`\\` / class / group / `^$`; not a lone `*`) | `RTX_TM_RE_SPAN` | `rtx_re_match_caps` at open, `rtx_re_match` (or a substituted literal closer) at close; `a`/`alen` and `b`/`blen` are pattern lengths |
| `match` = `X.*` with no metachar in `X` | `RTX_TM_LIT_LINE` | |
| `match` ∈ {`\b[0-9]+\b`, `[0-9]+`, `\d+`, `\b\d+\b`} | `RTX_TM_DIGITS` | |
| `match` = `\b(a\|b\|…)\b` | `RTX_TM_KEYWORDS` | |
| `match` = `\\.` | `RTX_TM_ESC_DOT` | |
| anything else | `RTX_TM_REGEX` | `tm_re.cch` engine; `captures` → `cap[1..7]` scopes |

Span nesting: `rtx_tm_try_rule` opens a `LIT_SPAN` when `allow_span` is
set. **Wedge 4:** `allow_span = 1` for inners and for guest top‑level
rules; `sp` exceeds 1. A span's `patterns` / `#include` attach via
`rtx_tm_add_inner_resolved` (depth cap 4), so `#italic` inside `#bold`
is a real inner `LIT_SPAN`. The current frame's closer is always tested
before inners or guest rules.
Inside a span whose inners are all `ESC_DOT` (or none), `rtx_tm_span_advance`
(`:1555`) `memchr`s to the closer's first byte; with any other inner the
generic loop at `:2811–2847` runs.

Emit: `rtx_tm_add_span` (`:1428`) pushes one `RtxRun` per span with
`hint_a`/`hint_b` = begin/end literal lengths (`core/document.cch:96–97`);
an unclosed span at window end is emitted with `hint_b = 0` (`:2870`).
Checkpoints (`rtx_tm_ckpt_add`, `:460`) store `sp` and the rule indices only
(`RtxTmCkpt`, `core/document.cch:60–64`), so a resumed span knows its rule but
not which grammar it belongs to.

## 1. Per-grammar inventory

Legend for "cannot": E = `include: source.x` embed, R = regex `begin`/`end`,
BC = `beginCaptures`/`endCaptures`, CN = `contentName`, I = `injections`,
W = `while`. None of the ten grammars uses any of E/R/BC/CN/I/W; the table
records what each would need to express its real-world embeds.

| Grammar | scopeName | fileTypes | Expresses today | Lowered ops | Cannot (needs) |
|---|---|---|---|---|---|
| `c` | `source.c` | c h ccs cch cc shcc hpp cpp | `//` line comment, `/* */` span, `"` span with `\\.` escape inner, `'` span, digits, keywords, `#directive` regex (list lacks `if`/`elif`/`else`/`undef`/`error`) | LIT_LINE, LIT_SPAN×3, ESC_DOT (inner), DIGITS, KEYWORDS, REGEX | `#if 0` disabled branch (R), `#define … \` continuation (R), `asm("…")` body (R, E, depth 2), printf placeholders (REGEX inner: allowed but disables the memchr fast path) |
| `css` | `source.css` | css | `//`, `/* */`, `"` span, `#hex` regex, unit-number regex, property keywords | LIT_LINE, LIT_SPAN×2, REGEX×2, KEYWORDS | `url(…)` / `'` strings (a), `@media` blocks (a), embedded in HTML via `<style>` (host side: R, E, CN) |
| `csv` | `text.csv` | csv tsv psv pipe | `#` line comment, `"` span with `""` inner, number/column/field/sep regexes | LIT_LINE, LIT_SPAN, REGEX×5 | nothing embed-related; note the `""` inner is `RTX_TM_REGEX` not `ESC_DOT`, so `"` spans take the slow path |
| `html` | `text.html.basic` | html htm | `<!-- -->` span, `"` span, `<style>`/`<script>` RE_SPAN with `source.css`/`source.js` guest, tag regex `</?[A-Za-z][^>]*>` | LIT_SPAN×2, RE_SPAN×2, REGEX | self-closing `<script … />` (needs lookaround), `style="…"`→CSS (R, E, CN), `<code class=language-x>` (R, E, CN, I), entities regex, `'` attribute strings (a) |
| `javascript` | `source.js` | js mjs ts | `//`, `/* */`, `"` span + `\\.` inner, `'` span, digits, keywords | LIT_LINE, LIT_SPAN×3, ESC_DOT, DIGITS, KEYWORDS | template literal `` ` `` (a for the outer span; `${…}` islands need depth 2 + host re-entry), tagged templates `sql`/`html`/`css`/`gql` (R, BC, CN, E), regex literal vs division (R with lookbehind), JSX/TSX (R, depth) |
| `json` | `source.json` | json | key regex with `captures.1`, `"` span + `\\.` inner, number regex, `true/false/null` | REGEX(has_cap), LIT_SPAN, ESC_DOT, REGEX, KEYWORDS | nothing embed-related (JSON is only ever a *guest*: YAML `>` scalars, md fences) |
| `markdown` | `text.markdown` | md markdown | ```` ``` ```` span, heading regex, `>` quote LIT_LINE, `**`/`*`/`` ` `` spans, link regex | LIT_SPAN×4, REGEX×2, LIT_LINE | fence info string → grammar (R, BC, CN, E), line-anchored fence close with backreference to the opener (R), blockquote continuation (W), list-item indentation (W), HTML blocks (E `text.html`), fence inside quote/list (depth 2) |
| `python` | `source.python` | py | `#` comment, `"` span, `'` span, digits, keywords | LIT_LINE, LIT_SPAN×2, DIGITS, KEYWORDS | `"""`/`'''` docstrings as a 3-byte span (a; today works by the `""`+`"…"`+`""` accident), SQL/regex/HTML in strings (E, CN, I), f-string `{…}` islands (depth 2 + host re-entry), `r"…"` prefix (BC) |
| `shell` | `source.shell` | sh bash | `#` comment, `"` span, `'` span, keywords | LIT_LINE, LIT_SPAN×2, KEYWORDS | heredoc `<<EOF` (R begin, R end with `\3` backreference), quoted vs unquoted heredoc (BC decides whether `$…` islands exist), `<<PYTHON`→`source.python` (CN, E), `$(…)`/`${…}` islands (depth 2), `<<<` must not match (R lookahead) |
| `yaml` | `source.yaml` | yml yaml | `#` comment, `"` span, number regex, `yes/no/true/…` keywords, `^key:` regex | LIT_LINE, LIT_SPAN, REGEX×2, KEYWORDS | block scalars `\|`/`>` (W: body is indentation-delimited, no closer), `'` strings (a), flow `{}`/`[]` (a), anchors/aliases/`---` (REGEX, cheap), embeds by key name `run: \|`→shell (I) |

Observations that cut across grammars:

- Every `"` / `'` string is a 1-byte `LIT_SPAN`. The walker's backslash skip
  (`:1567–1590`, `:2830`) makes escapes work without an inner rule, and the
  memchr closer makes triple quotes "work" as empty+full+empty. Any change to
  string spans must keep those two accidents or replace them deliberately.
- Comments are tried before strings in every grammar, so a `"` inside a
  comment or a `/*` inside a string is already handled correctly
  (`embeds.c` lines 20–22, `embeds.py` line 32).
- HTML `<style>` / `<script>` are the first regex `begin`/`end` rules;
  `rtx_tm_lower_rule` chooses `RE_SPAN` when a metachar other than a lone
  `*` is present. `RTX_TM_REGEX` remains the leftover `match` path.

## 2. Fixture → TextMate construct → lowering needs

Needs codes:

- **(a)** literal `begin`/`end` is enough (today's `RTX_TM_LIT_SPAN`).
- **(b)** needs regex `begin`/`end` (anchors, alternation, backreference from
  `begin` captures into `end`, lookahead/lookbehind).
- **(c)** needs `include: source.x` resolved to another *loaded* grammar
  (requires reading `scopeName` and a registry lookup by scope, not by extension).
- **(d)** needs `contentName` (the body gets a different scope / grammar than
  the delimiters; begin/end stay in the host).
- **(e)** needs span depth > 1 (a guest span inside a host span, or a host
  island inside a guest span).
- **W** = needs `while` (indentation / prefix continuation), which none of
  a–e covers; listed where it applies.

| Fixture (lines) | Real-world TextMate construct | Needs |
|---|---|---|
| `embeds.py` 1–4, 16–20 docstrings | `string.quoted.docstring.multi.python` — `begin: (\"\"\")`, `end: (\"\"\")`, `beginCaptures` on the quote, inner `patterns` for escapes/doctests | (a) for the span; BC only for punctuation scope |
| `embeds.py` 7–12 SQL in `"""` | same docstring span with an *injection* (`injectionSelector: L:string.quoted.docstring`) or a `contentName: meta.embedded.sql` + `include: source.sql` in a custom rule; no stock grammar does it | (a) span, (c), (d) |
| `embeds.py` 21 f-string | `string.interpolated.python`: `begin: (?i)(f)(")`, `end: (")`, inner `meta.fstring` `begin: \{`, `end: \}` with `include: $self` | (b) for the prefix, (e) + `$self` re-entry for the islands |
| `embeds.py` 26 raw regex | `string.regexp.quoted.single.python`: `begin: (?i)(r)(")`, `contentName: source.regexp.python`, `include: source.regexp.python` | (b), (c), (d) |
| `embeds.py` 30–33 escapes / nested quotes | `constant.character.escape.python` inner `\\.` | (a) — already works |
| `embeds.js` 3 template literal | `string.template.js`: `begin: `` ` ``, `end: `` ` ``, inner `meta.template.expression.js` `begin: \$\{`, `end: \}` with `include: $self` | (a) outer; (e) + `$self` for `${}` |
| `embeds.js` 5 `` sql`…` `` | `string.template.js` with `begin: (?:(sql)\s*)(`)`, `beginCaptures`, `contentName: meta.embedded.sql`, `include: source.sql` | (b), BC, (c), (d), (e) for `${}` |
| `embeds.js` 6 `` html`…` `` | same shape, `contentName: meta.embedded.block.html`, `include: text.html.basic` | (b), BC, (c), (d) |
| `embeds.js` 8–10 regex vs division | `string.regexp.js`: `begin: (?<=[=(,:;!&\|?{}\[]\|return\|^)\s*(/)(?![/*])`, `end: (/)([gimsuy]*)` | (b) with lookbehind/lookahead |
| `embeds.js` 12–13 JSX/TSX (in a comment here) | `meta.tag.tsx` `begin: (<)([A-Z]\w*)`, `end: (/>)\|(</\2>)`, attribute strings, `{…}` islands via `include: $self` | (b) with backreference, (e) |
| `embeds.js` 15–16 backticks in strings | `"`/`'` spans | (a) — already works |
| `embeds.js` 18–29 multi-line template with a fence | `string.template.js` span; the escaped ``\` `` is `constant.character.escape.js` `\\.` inner | (a) + ESC_DOT inner (the fence lookalikes are then automatically body) |
| `embeds.html` 5–9 `<style>` | `begin: <style\\b[^>]*>`, `end: </style\\s*>`, `include: source.css` (no lookaround, so self-closing is leftover) | **landed** as `RE_SPAN` + lex-time embed; `contentName` / `beginCaptures` still skipped |
| `embeds.html` 10–14 `<script>` with split closer | same with `source.js`; host closer is always tested first, so `"</scr" + "ipt>"` stays inside the JS string | **landed** (depth 2); (d) `contentName` still skipped |
| `embeds.html` 16 `style="…"` | `meta.attribute.style.html`: `begin: (style)\s*(=)\s*(")`, `end: (")`, `contentName: meta.embedded.line.css`, `include: source.css` | (b), (c), (d) |
| `embeds.html` 17 comment containing `<script>` | `comment.block.html` `<!--`/`-->` | (a) — already works |
| `embeds.html` 18–21 `<pre><code class="language-python">` | no stock rule; a custom `begin: (<code\b[^>]*class="language-(\w+)"[^>]*>)`, `end: (</code>)`, `contentName` chosen from capture 2 | (b), BC (dynamic grammar from a capture), (c), (d) |
| `embeds.html` 22–24 `<svg>` | tags + attribute strings (`text.xml` embed exists in some grammars but not VS Code's) | (a)/REGEX — no embed required |
| `embeds.html` 25–28 entities | `constant.character.entity.html` `match: &([a-zA-Z0-9]+\|#[0-9]+\|#x[0-9a-fA-F]+);` | REGEX match — cheap, no span |
| `embeds.sh` 5–8 `<<EOF` | `string.unquoted.heredoc.shell`: `begin: (<<)-?\s*("\|')?\s*([^;&<\s]+)\s*\2`, `end: ^\s*\3\s*$`; `$…` islands via `include: #interpolation` | (b) with `\3` backreference in `end`, (e) for islands |
| `embeds.sh` 10–13 `<<'EOF'` | same `begin`, `string.quoted.heredoc.shell`, **no** inner patterns (quoting decided by capture 2) | (b), BC (the capture selects the body rule set) |
| `embeds.sh` 16–19 `<<-EOF` | same `begin` (the `-?`), `end: ^\t*\3\s*$` | (b) |
| `embeds.sh` 22–28 `python3 - <<PY` | `begin: (<<)-?\s*("\|')?\s*(PYTHON)\s*\2` … `contentName: source.python`, `include: source.python` (VS Code keys `RUBY`/`PYTHON`/`APPLESCRIPT`/`SHELL`; `PY` needs an alias) | (b), (c), (d), (e) (docstring at depth 2) |
| `embeds.sh` 30–35 `case` patterns | `meta.case.shell` `begin: \bcase\b`, `end: \besac\b`, pattern `match` `[^)]+\)`; `"$0"` strings with `variable.other` islands | (a) for `case…esac` (keywords already work); islands (e) |
| `embeds.sh` 37 `<<<` | `keyword.operator.herestring.shell` `match: <<<`; heredoc `begin` must not match (`(<<)(?!<)`) | (b) lookahead (or rule order: try `<<<` before `<<`) |
| `embeds.yaml` 4–10 `install: \|` | `string.unquoted.block.yaml`: `begin: (?:(\|)\|(>))([1-9])?([-+])?(.*\n?)`, **`while: ^([ ]{N,})`** — no `end`; body is indentation-delimited | W (not covered by a–e); body embed only via I keyed on the key name |
| `embeds.yaml` 11–14 `config: >` | same block-scalar rule, folded | W, I (`source.json`) |
| `embeds.yaml` 15–23 markdown + fence | same; body `text.markdown` → fence → `source.python` | W, (c), (d), (e) (depth 3) |
| `embeds.yaml` 24–26, 31 anchors / alias / merge | `match: (&)([^\s]+)`, `match: (\*)([^\s]+)`, `match: <<:` | REGEX — cheap |
| `embeds.yaml` 27 flow mapping | `meta.flow-mapping.yaml` `begin: \{`, `end: \}`, inner flow-key/flow-value rules | (a) for the braces; inner key regex without `^` |
| `embeds.yaml` 28, 34 `---` | `entity.other.document.begin.yaml` `match: ^---` | REGEX — cheap |
| `embeds.c` 4–8 macro continuation | `meta.preprocessor.macro.c`: `begin: ^\s*((#)\s*define)\b`, `end: (?<!\\)(?=\n)` | (b) with lookbehind |
| `embeds.c` 13–15 `asm volatile("…")` | (cpp grammar) `meta.asm.c`: `begin: (\b(?:__)?asm(?:__)?\b)\s*(volatile\|__volatile__)?\s*(\()`, `end: \)`, strings inside as `meta.embedded.assembly` | (b), (c) if an asm grammar is loaded, (e) (strings at depth 2) |
| `embeds.c` 20–23 string/comment/char/printf | `"`/`'`/`/* */` spans; `constant.other.placeholder.c` `match: %[-+ #0]*[0-9]*(\.[0-9]+)?[hlLqjzt]*[diouxXeEfFgGaAcspn%]` as a string inner | (a) — already works; placeholder is a REGEX inner |
| `embeds.c` 25–28 `#if 0` | `comment.block.preprocessor.if-branch.c`: `begin: ^\s*((#)\s*if)\s+(0)\s*(?=$\|//\|/\*)`, `end: ^\s*((#)\s*(endif\|else\|elif))\b` | (b) with `^` anchors (also fixes today's mis-spanned `"` on 26–27) |
| `nested.md` 7–14, 18–23, 27–34, 38–43 fences | `markup.fenced_code.block.markdown`: `begin: (^\|\G)(\s*)(`{3,}\|~{3,})\s*(?i:(python\|py)((\s+\|:\|,\|\{\|\?)[^`]*)?$)`, `beginCaptures.5: fenced_code.block.language`, `end: (^\|\G)(\2\|\s{0,3})(\3)\s*$`, `contentName: meta.embedded.block.python`, `include: source.python` | (b) with `\2`/`\3` backreferences, BC (info string), (c), (d), (e) (guest spans at depth 2–4) |
| `nested.md` 47–54 fence in blockquote | `markup.quote.markdown`: `begin: (^\|\G)[ ]{0,3}(>) ?`, **`while: (^\|\G)\s*(>) ?`**, `include: #block` | W, (e) |
| `nested.md` 58–67 fence in list item | `markup.list.numbered.markdown`: `begin: (^\|\G)([ ]{0,3})([0-9]+\.)\s`, **`while: ((^\|\G)([ ]{2,4}\|\t))`** | W, (e). Today's unanchored literal ```` ``` ```` happens to catch the indented fence |

## 3. Minimum lowering additions, cheapest first

Ordered by how little of the walker/lowering has to change. Each item names
the fixture that proves it and where it lands.

1. **Line-anchored fence close + info string (Markdown fences).** **Landed
   2026‑09‑05** (`cctext.bol` + `cctext.info`; guest resolved at lex via
   `rtx_tm_rt_for_info`). Historical note, kept for the leftover ≤3‑space
   / trailing‑whitespace gap: extend
   `RTX_TM_LIT_SPAN` with a "closer must be at line start (after ≤3 spaces)
   and followed by only whitespace" flag, and record the run of bytes after
   the opener up to EOL as the info string. This fixes `nested.md` line 11
   (docstring ```` ``` ```` closing the fence) without a regex engine change.
   Lands in: `rtx_tm_lower_rule` (`:1340`, set the flag when `begin == end ==`
   ```` ``` ````, or from a `cctext` sidecar key), `rtx_tm_span_advance`
   (`:1555`, check line start / trailing whitespace before accepting `hit_end`),
   and the closer test in the generic loop (`:2834–2846`). The info string is
   the hook for item 3; store it on the span's `RtxRun` (`core/document.cch:96–97`
   already carries `hint_a`/`hint_b`) or resolve it immediately.
2. **Multi-byte string openers with escape (Python docstrings).** **Landed
   2026‑09‑05** (`#ddstring` / `#dsstring` before `"` / `'` in
   `python.tmLanguage.json`). Add explicit
   `"""` / `'''` `LIT_SPAN` rules ahead of `"` / `'` in `python.tmLanguage.json`
   with a `\\.` inner. Pure grammar edit; the lowering already handles a 3-byte
   `begin`/`end` (`rtx_tm_starts`, `:1316`). Removes the `""`+`"…"`+`""` accident
   in `embeds.py` 1–4, 7–12, 16–20. Not a lowering change, but it is the
   prerequisite for injecting SQL at 7–12.
3. **`include: source.x` → loaded grammar (the embed itself).** **Landed
   2026‑09‑05** (`RtxTmRt.scope` / `title`, `rtx_tm_rt_for_scope()`,
   `embed_scope` on the span, info‑string resolve at lex — no
   `RTX_TM_EMBED` op). Read
   `scopeName` in `rtx_tm_load_json` (new branch beside `fileTypes` at `:2118`;
   today it falls into `rtx_jw_skip` at `:2171`) and store it on `RtxTmRt`
   (`core/tm.cch:61–67`). Add `rtx_tm_rt_for_scope()` next to
   `rtx_tm_rt_for_ext` (`:2370`). In `rtx_tm_flatten` (`:2030–2043`) a pattern
   whose `include` is not `#…` becomes a new op, say `RTX_TM_EMBED`, carrying
   the target scope (interned on the TM store like `RtxTmPat.incl`,
   `core/tm.cch:81`). For fences the target is chosen at run time from the
   info string of item 1 (`python` → `source.python`, via each grammar's
   `fileTypes`/`name`), so the fence rule needs a "resolve from info string"
   variant rather than a fixed scope. Covers `nested.md` fences and, with
   item 4, everything else.
4. **`contentName` + guest lexing inside a span (depth 2).** **Landed
   2026‑09‑05** without `contentName`: guest from `cctext.info` /
   `embed_scope`; `stk_lang` / `stk_embed` on the walk and in
   `RtxTmCkpt`; closer first; `RTX_RUN_INJECT`. `contentName` itself is
   still skipped. Read
   `contentName` in `rtx_jw_pattern_fields` (new branch beside `include` at
   `:1867`; add a field to `RtxTmPatRaw` `:1605–1618`, `RtxTmPat`
   `core/tm.cch:76–89`, `RtxTmRule` `:40–59`). In `rtx_tm_lex`, when the top
   frame is a span with an embed target, run the *guest* `RtxTmRt`'s rules over
   the body instead of `fr->innern` (`:2811–2847`), while still testing the
   host closer first at each position so `</script>` closes the guest's
   unclosed strings (the `embeds.html` 11 case) and the fence closer wins over
   a guest docstring. Needs the stack to carry the grammar per frame
   (`stk_rule`/`stk_off` at `:2788`; `RtxTmCkpt.rule[]`
   `core/document.cch:63` must gain a grammar index or encode it in the
   `unsigned short`). Set `allow_span = 1` for guest rules so a guest span can
   open: this is where `sp` first exceeds 1 (`core/tm.cch:69–73`). Covers
   Python docstrings + SQL (`embeds.py` 7–12), HTML `<script>`/`<style>` once
   item 5 gives them a regex begin, and all of `nested.md` depth-2 rows.
5. **Regex `begin`/`end` (HTML script/style, JS templates, heredocs).**
   **Landed 2026‑09‑05** for HTML `<script>` / `<style>`: `RTX_TM_RE_SPAN`
   when `begin` or `end` has a regex metachar (not a lone `*`), matched
   with `rtx_re_match_caps` / `rtx_re_match`. Substituted closer hook:
   `\\1`–`\\7` from the begin caps become a per-frame literal
   (`stk_close` / `stk_closen`); closer test uses `rtx_tm_starts`. Not on
   `RtxTmCkpt` — mid-heredoc window resume is leftover. `beginCaptures`
   writes the same `capn[]` as `captures` (no separate emit). Engine still
   has no lookaround / backrefs / `\\G` / `(?i:)`. `span_advance` memchr
   stays LIT_SPAN-only. Still leftover: self-closing `<script … />`,
   `style="…"`, `<code class=language-x>`, JS templates, heredoc product
   work (hook only), `embeds.c` `#if 0` / macro continuation.

Not in the minimum set: `while` (YAML block scalars, Markdown blockquotes and
list items — `embeds.yaml` 4–23, `nested.md` 47–67), `injections` /
`injectionSelector` (key-name embeds in YAML, `<code class=…>` in HTML), and
host-language islands inside a guest (`${}` in JS templates, `$…` in unquoted
heredocs, `{…}` in f-strings) which need a `$self` re-entry on top of item 4.
The `cctext` sidecar already parsed by the walker (`RtxTmRule.sc_*`,
`core/tm.cch:52–58`) is a cheap place to carry an `"embed": "source.x"` hint
until items 3–5 land, and to alias `PY` → `source.python` for the shell
heredoc fixture.

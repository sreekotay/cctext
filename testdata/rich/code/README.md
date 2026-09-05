# testdata/rich/code — embedded-language fixtures

Fixtures for "grammar injection at a span": a host file whose spans contain
another language that should get its own highlight (and later its own
atoms/marks). Every file is small, deterministic, LF-only, < 4 KiB. Line
numbers below were checked with `nl -ba`; re-run it if a fixture is edited.

"Today" describes what `core/document.ccs` (`rtx_tm_lex` over the lowered
table in `core/tm.cch`) does with the shipped `testdata/grammars/*.tmLanguage.json`.
"Expected" is the target behaviour once the span is injected. The needs codes
(a–e) are defined in [docs/grammar_audit.md](../../../docs/grammar_audit.md).

| File | Bytes | Lines | Exercises |
|---|---:|---:|---|
| `embeds.py` | 747 | 33 | docstrings, SQL in `"""`, f-string, raw regex, escaped `"""`, nested quotes |
| `embeds.js` | 844 | 31 | template literals, tagged `sql`/`html`, regex vs division, JSX in a comment, backticks in strings, md fence inside a template |
| `embeds.html` | 938 | 30 | `<style>`, `<script>` with split `</scr"+"ipt>`, inline `style=`, `<pre><code class=language-python>`, comment with `<script>`, `<svg>`, `<details>`, entities |
| `embeds.sh` | 784 | 38 | `<<EOF`, `<<'EOF'`, `<<-EOF` (tabs), heredoc with `$(…)`/`${…}`, `python3 - <<PY`, `case`, `<<<` |
| `embeds.yaml` | 662 | 36 | `|` and `>` block scalars (shell, JSON, markdown+fence), `---` multi-doc, anchors/aliases, flow mapping |
| `embeds.c` | 956 | 36 | `asm volatile("…")`, `/* */` in a string, `"` in a comment, `\` macro, `#if 0` with unbalanced `"`, printf format, `'"'` |
| `nested.md` | 1034 | 69 | fences embedding each of the above (depth 2–3), fence in blockquote, fence in list item |

## embeds.py

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 1–4 | module docstring `"""…"""` | one `string.quoted.docstring.multi.python` span 1:1–4:3; line 3's ``` ``` ``` and `def` are body text | `"` is a 1-byte LIT_SPAN, so it lowers as `""` + `"…"` + `""`; still all string, by accident |
| 7–12 | `SQL = """SELECT … WHERE …"""` | string span 7:7–12:3 with body 8–11 injected as `source.sql` (needs (c)+(d); no SQL grammar is shipped, so falls back to plain string). Line 11's `'%o''brien%'` is SQL text, never a Python `'` string | same `""`/`"…"`/`""` accident; `'` ignored inside the `"` span (correct) |
| 16–20 | function docstring | docstring span 16:5–20:7; line 18–19 doctest could be a further `source.python` embed (not required) | accidental triple split, all string |
| 21 | f-string | `string.interpolated.python`; `{name!r}` and `{len(name) + 1}` are host-language islands, `{{literal braces}}` is text | `f` consumed by KEYWORDS as an identifier, then plain `"` string; no islands |
| 25 | `'''…'''` docstring with `"double"` inside | one span; the inner `"` never opens a string | `'` 1-byte span; the `"` is skipped inside (correct) |
| 26 | `r"^\d+\.\s+(\w+)\s*$"` | `string.regexp.python` (raw string → regex embed, (c)) | `"` span; the `\` skip keeps `\.` etc. inside (correct) |
| 30 | `"a \"\"\" b"` | escaped quotes are `constant.character.escape`; the span closes at the last `"` on line 30 | 1-byte closer with `\`-skip: closes correctly |
| 31 | `"she said 'it''s \"fine\"'"` | one string; inner `'` are text | correct |
| 32 | comment containing `def` and `"""` | comment 32:1–EOL; nothing inside is a keyword or string | `#.*` LIT_LINE wins first (correct) |
| 33 | `'…"…'` + `"…'…"` | two strings, each closer chosen by its own quote kind | correct |

## embeds.js

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 3 | `` `hello ${name} and ${1 + 2} done` `` | `string.template.js` 3:15–3:47 with two `meta.template.expression` islands (`${…}`) lexed as JS: a host island inside a span (needs (e)) | no backtick rule: the whole line is plain JS, `name` etc. unstyled |
| 5 | `` sql`SELECT … ${userId} …` `` | tag `sql` + template body as `source.sql` (VS Code: `string.template.js` with `meta.embedded.line.sql`; needs (b) for the tag regex, (c), (d)); `${userId}` is a JS island | plain JS; `SELECT` not a keyword |
| 6 | `` html`<div class="card">…</div>` `` | body as `text.html.basic`; `class="card"` becomes an HTML attribute string, not a JS string | `"card"` is a JS `"` string (accidentally fine) |
| 8 | `/ab+c/gi` | `string.regexp.js` 8:12–8:19 | no regex rule; unstyled |
| 9–10 | `a / b / c`, `total / count` | division: must NOT open a regex span; the `//` on line 10 is a comment | correct (no regex rule to misfire) |
| 12–13 | `// <Foo bar="x">{y}</Foo>` (TSX-style, in a comment) | comment only. In a `.tsx` file the same text is `meta.tag.tsx` with `{y}` as a JS island (needs (b),(e)) | LIT_LINE comment (correct) |
| 15 | `"string with `backticks` inside"` | one `"` string; backticks are text | correct |
| 16 | `'and \'single\' with ` too'` | one `'` string with two escapes; the backtick is text | correct (`\`-skip) |
| 18–29 | multi-line template containing a markdown fence | one `string.template.js` 18:13–29:16; lines 19–28 are template body, so line 19 `# Title` is not a comment and line 26 `return` is not a keyword; the escaped ``\` `` on 23 and 27 must not close the template | plain JS: `return` (26) styled as keyword; nothing else |
| 31 | `export { … }` | keywords | `export` keyword (correct) |

## embeds.html

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 5–9 | `<style>…</style>` | span 5:3–9:10, `contentName: source.css` over lines 6–8 (needs (b),(c),(d)). Line 7 `"<style>"` is a CSS string and must not re-open a style span; line 8's `<script>` is inside a CSS comment and must not open a script span | `<style>`/`</style>` match the `#tag` regex; the body is lexed as HTML: `"<style>"` becomes an HTML string, `/* … */` unstyled |
| 10–14 | `<script>…</script>` | span 10:3–14:11, body 11–13 as `source.js`. Line 11's `"a string with </scr" + "ipt> …"` is two JS strings and must not close the span; the real closer is line 14 | tag regex on `<script>`/`</script>`; body lexed as HTML (`"` strings only) |
| 16 | `style="background: #fff; padding: 1em"` | attribute value injected as `source.css` (VS Code: `meta.embedded.line.css`; needs (b),(c),(d)) | `"` string |
| 17 | `<!-- <script>alert("in a comment")</script> -->` | one comment 17:3–17:49; the inner `<script>` never opens a span | `<!--` LIT_SPAN opens first (correct) |
| 18–21 | `<pre><code class="language-python">…</code></pre>` | body from 18:38 to the end of line 20 as `source.python` (docstring on 19). No stock grammar does this; it is an injection keyed on the `class` attribute (needs (b),(c),(d)) | tags + `"language-python"` string; body plain |
| 22–24 | inline `<svg>` | tags/attributes; no embed in stock HTML grammars (SVG is HTML here) | tags + attribute strings |
| 25–28 | `<details><summary>` + entities | tags; `&amp;` `&lt;` `&gt;` `&copy;` as `constant.character.entity.html` (regex match, no span) | tags only; entities unstyled |

## embeds.sh

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 5–8 | `cat <<EOF … EOF` | `string.unquoted.heredoc.shell` 5:5–8:3; body 6–7. `${NAME}` and `$(date +%Y)` on line 6 are host islands (needs (e)); `\$ESCAPED` on 7 is an escape, `"def"` is text | no heredoc rule; line 7's `"def"` is a string, line 6 plain |
| 10–13 | `cat <<'EOF' … EOF` | `string.quoted.heredoc.shell`; `$(this)` / `${that}` on 11 are NOT islands; `"this"` / `'that'` on 12 are text | `'EOF'` string; line 12 two strings |
| 16–19 | `<<-EOF` with tab-indented body and closer | heredoc whose closer is `^\t*EOF$` (needs (b): regex end with backreference to the delimiter) | nothing |
| 22–28 | `python3 - <<PY … PY` | heredoc 22:11–28:3 with body 23–27 as `source.python` (VS Code's shell grammar keys `contentName` on delimiters like `PYTHON`; `PY` needs an alias). Line 25 is a Python docstring inside the heredoc: depth 2 (needs (b),(c),(d),(e)) | `"""docstring…"""` is `""` + `"…"` + `""` shell strings; `"hello from python"` string |
| 30–35 | `case … in` with `-h|--help)`, `*.py|*.js)`, `"")` | `keyword.control.shell` `case`/`in`/`esac`; patterns are `meta.case.shell` text; `|` here is not a pipe; `"$0"`/`"$1"` on 31–34 are strings with variable islands | keywords + `"` strings; `""` on 33 is an empty string (correct) |
| 37 | `<<<"one two three"` | here-string: `<<<` operator + string; must NOT be treated as a heredoc opener | `"` string (correct) |

## embeds.yaml

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 4–10 | `install: \|` block scalar | `string.unquoted.block.yaml` 4:10–10:5; the body ends by *indentation*, not a delimiter (TextMate `while`). Body as `source.shell` only via an injection keyed on the key name (GitHub-Actions style); line 8–10 heredoc inside → depth 2 | line 5 `#!/bin/sh` and line 16 `# Markdown heading…` are styled as YAML comments (wrong); `"installing …"` string |
| 11–14 | `config: >` folded scalar with a JSON blob | block scalar 11:9–14:36; body as `source.json` by key-name injection | `"key"` etc. are YAML strings (accidentally right-ish) |
| 15–23 | `notes: \|` with markdown + a python fence | block scalar 15:8–23:5; body as `text.markdown`; the fence 20–23 then embeds `source.python` → depth 3 (yaml→md→py) (needs (e), `while`) | line 16 is a comment (wrong); line 22 `"""docstring"""` is `""`+`"…"`+`""` |
| 24–26 | `defaults: &defaults` | `entity.name.type.anchor.yaml` on `&defaults` (regex match) | key regex matches `defaults:`; anchor unstyled |
| 27 | `flow: {a: 1, b: "two", c: [3, 4]}` | `meta.flow-mapping.yaml` 27:7–27:33 (literal `{`/`}` span, (a)); `a:` `b:` `c:` are flow keys | key rule is `^`-anchored so only `flow:` is a key; `"two"` string; digits |
| 28, 34 | `---` | `entity.other.document.begin.yaml` (regex match `^---`) | unstyled |
| 31 | `<<: *defaults` | merge key + `variable.other.alias.yaml` on `*defaults` | `<<:` is not matched by the key regex; unstyled |
| 33 | `enabled: yes` | `constant.language.boolean.yaml` | `yes` keyword (correct) |
| 36 | bare scalar document | plain | plain |

## embeds.c

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 4–8 | `#define SWAP(a, b) do { \` … `} while (0)` | `meta.preprocessor.macro.c` 4:1–8:11: a line span that continues while the line ends in `\` (regex end `(?<!\\)$`, needs (b)); `while` on 8 is still a keyword inside it | `#define` directive matched; each line lexed independently |
| 13–15 | `asm volatile("rdtsc\n\t" "mov %%eax, %0" : "=r"(lo) : : "edx")` | `meta.asm.c` span 13:5–15:38 with the string bodies as assembly (`source.asm` if a grammar exists, (c)); strings nested in the asm span → depth 2 (e). `\n\t` are escapes | `asm`/`volatile` not keywords; strings + `\n`/`\t` escapes (correct as far as it goes) |
| 20 | `"/* this is a string, not a comment */"` | string; no comment opens | `"` matched before `/*` inside is reached; span memchrs to the closing `"` (correct) |
| 21 | `/* … " … ' … */` | comment; no string opens | `/*` LIT_SPAN; `"` skipped (correct) |
| 22 | `'"'` | `string.quoted.single.c` 22:32–22:34; the `"` is body | `'` 1-byte span closes at 22:34 (correct) |
| 23 | `"%s=%d %5.2f\t%%\n"` | string with `constant.other.placeholder.c` on each `%…` and escapes on `\t`, `\n` (inner regex match inside a span) | escapes styled; placeholders not |
| 25–28 | `#if 0` … unbalanced `"` … `#endif` | `comment.block.preprocessor.if-branch.c` 25:1–28:6 (VS Code cpp grammar; regex begin `^\s*#\s*if\s+0\b`, end `^\s*#\s*(endif|else|elif)`, needs (b)); the `"` on 26 and 27 are comment text | `if` is not in the directive list, so `#if 0` is unstyled, `0` is a digit; the `"` on 26 opens a string that closes at the `"` on line 27 — lines 26–27 are mis-spanned as one string, then `#endif` is a directive |
| 32–34 | `SWAP(a, b)`, `printf(...)` | keywords / strings | correct |

## nested.md

| Lines | Construct | Expected | Today |
|---|---|---|---|
| 7–14 | ```` ```python ```` fence with a docstring | fence 7:1–14:3, info string `python` → body 8–13 as `source.python`; the docstring 9–12 is a Python span inside (depth 2). Line 11's ``` ``` ``` is docstring text and must NOT close the fence: fence close must be line-anchored (`^\s*```\s*$`, needs (b)) | the literal ``` ``` ``` LIT_SPAN closes at the ``` ``` ``` on line 11; 14:1 re-opens a fence that runs to 18:3, so lines 14–18 (`## HTML fence…`) are raw and the html fence is inverted. First visible bug this fixture pins |
| 18–23 | ```` ```html ```` fence with `<script>` | body 19–22 as `text.html.basic`; 19:1–22:9 is a script span with body as `source.js` (depth 3: md→html→js). Line 20's split `"</scr" + "ipt>"` must not close the script; line 21's template with `${s}` is a JS island | see above: fence phase is inverted from line 14 on |
| 27–34 | ```` ```js ```` fence with a template literal | body 28–33 as JS; the template 28:13–33:1 is a JS span (depth 3 md→js→template). The escaped ``\` `` on 30 and 32 must close neither the template nor the fence; line 29 `# not a markdown heading` is template text | inverted phase; line 29 `#…` may be styled as a heading depending on the phase |
| 38–43 | ```` ```sh ```` fence with `<<PY` heredoc | body 39–42 as `source.shell`; heredoc 39:11–42:3 with body 40–41 as `source.python`; the docstring on 41 is depth 4 (md→sh→py→docstring) | phase-dependent |
| 47–54 | fence inside a blockquote | quote span 47–54 (`markup.quote`, TextMate `while` continuation on `^>`); nested fence 49:3–52:5 with body 50–51 as `source.python` (needs `while`, (e)) | `>.*` LIT_LINE wins at line start: 47–54 are all quote lines, the fence is never seen |
| 58–67 | fence inside a list item (3-space indent) | list item 59–65; fence 61:4–65:6 with body 62–64 as `source.shell`; the heredoc 62–64 is depth 3 | the literal ``` ``` ``` matches at any column, so 61–65 is a fence (correct) once the phase above is right |
| 69 | trailing prose | plain | plain |

## What the fixtures pin, in one sentence each

- `embeds.py`: the 1-byte `"` span gets triple quotes right by accident; SQL/regex/f-string islands need (c)/(d)/(e).
- `embeds.js`: JS has no template rule at all; templates, tags, regex-vs-division all need (b) and template islands need (e).
- `embeds.html`: `<script>`/`<style>`/`style=`/`<code class>` are the canonical (b)+(c)+(d) case; the split `</scr"+"ipt>` is the closer-inside-body trap.
- `embeds.sh`: heredocs are (b) with an end-regex backreference; `<<PY` adds (c)+(d); `<<<` must not match.
- `embeds.yaml`: block scalars end by indentation (TextMate `while`), which none of (a–e) covers; embeds are by key-name injection, not by grammar.
- `embeds.c`: `#if 0` (b) and macro continuation (b); everything else already works with literal spans.
- `nested.md`: line 11 shows the literal-fence closer bug; blockquote fences need `while`; everything else is depth ≥ 2.

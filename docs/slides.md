# Slides: Marp decks over bytes

A Markdown file whose front matter says `marp: true` is a
[Marp](https://marp.app) deck. cctext edits it as the Markdown it is and
shows it as slides: `Shift-F5` (**Present Slides**) fills the window
(cctext-ui) or the terminal (cctext) with the slide under the caret,
steps through build steps, animates the move between slides, and `Esc`
comes back to the editor on the slide that was showing.

Bytes are truth. A deck is a *view* of the document: no slide store, no
rendered copy, no second document. Every slide boundary, directive, build
step and transition is read from the bytes when it is needed, and editing
never goes through the presentation. Same rule as the Rich lens
([md_view.md](md_view.md)): a run, a layout policy over runs, or a
leftover — never a wrong answer.

Code: `core/slides.cch` / `slides.ccs` (deck index, directives, one slide
as styled lines, the presenter state), `core/md_block.ccs`
(`RTX_MDB_MARP`, `RTX_MDB_S_SLIDE`), `frontend/gui_present.ccs`
(cctext-ui), `frontend/cctext_present.ccs` (cctext). Sample deck:
`testdata/slides/demo.md` (every feature below).

## Marp compatibility scope

| Marp | cctext |
|---|---|
| Front matter `marp: true` | Required. Read from the first 4 KiB (`RTX_MDB_MARP_PROBE`), YAML `---` … `---` / `...` at BOF, `marp: true` on its own line (any case, a trailing `# comment` is fine). Without it nothing below applies and `---` is an ordinary thematic break. |
| Slide separator `---` | Any **top-level** thematic break (`---`, `***`, `___`, `- - -`), as Marpit splits on top-level `hr` tokens. A break inside a block quote or list item is a rule on that slide. `---` inside a fence, HTML block, math or indented code is body text. `Title` + `---` with no blank line between is a **setext heading**, because that is what markdown-it (and so Marp) reads — leave a blank line before a separator, or use `***`. |
| `headingDivider: N` / `[a, b]` | Supported in the deck index: a top-level ATX heading of level ≤ N (or a listed level) starts a new slide unless the slide has no content yet. Setext headings do not divide. |
| Global directives | `theme` (`default`, `gaia`, `uncover` palettes; any other name draws as `default`), `size` (`16:9` = 1280×720, `4:3` = 960×720; others 16:9), `headingDivider`. `style`, `lang`, `math` and Marp CLI metadata (`title`, `description`, `author`, `image`, `keywords`, `url`) are recognised and not drawn. The last definition wins, as in Marpit. |
| Local directives | `paginate` (`true` / `false` / `hold` / `skip`), `header`, `footer`, `class` (`lead` centres, `invert` swaps to the dark palette), `backgroundColor`, `color` (`#rgb`, `#rrggbb`, ~50 CSS names), `transition`. They apply to their slide and every later one; front matter sets them from slide 1. `backgroundImage` / `-Position` / `-Repeat` / `-Size` are recognised, not drawn. |
| Spot directives `_key` | Apply to their slide only (`<!-- _class: lead -->`). |
| HTML comment directives | `<!-- key: value -->`, one or many `key: value` lines, block level. A comment that is not all directives is a presenter note: not drawn, not applied. Inline comments are not read. |
| `![bg](url)` | Background image of its slide. Keywords in the alt text: `bg`, `left` / `right` with an optional `:N%` (split background: the content keeps the other side), `fit` / `contain`, `cover`, `auto`. Several `bg` images are counted; the first is drawn. cctext-ui draws no bitmaps: a split background is a tinted, framed panel with the file name, a full one a frame and a label. The terminal hatches the split side. |
| `![alt](url)` | A content image is a line of its own: a labelled frame (cctext-ui) or `[image url]` (cctext). Size keywords (`w:320`) are read as alt text. |
| Fragmented lists | `*` bullets and `1)` ordered items are build steps (Marp's fragmented list); `-`, `+` and `1.` are always shown. Everything inside a stepped item (continuation lines, nested non-stepped items) appears with it. Hidden steps keep their space, as Marp's inactive fragments do. |
| `transition` (Marp CLI) | `none`, `fade`, `fade-out`, `slide`, `push`, `cover`, `reveal`, `wipe`, `zoom` are drawn as named; the rest of Marp CLI's set maps to the nearest (`wiper`, `melt`, `clockwise` → wipe; `cube`, `cylinder`, `swap` → push; `swoosh` → slide; `pull` → reveal; `drop` → cover; `implode`, `explode`, `iris-in/out`, `diamond`, `star` → zoom; `in-out` → fade-out; `overlap`, `glow`, `flip`, `pivot`, `rotate` → fade). An unknown name is a fade. `morph` / `magic-move` are cctext names (below). A duration follows the name: `fade 0.5s`, `push 400ms` (default 300 ms). |
| Markdown | Everything the block pass and the Rich lens know: headings, paragraphs (every newline breaks, as Marp's `breaks: true`), lists with task boxes, block quotes, fences with the info string's grammar, indented code, `$$` math (as code), GFM tables with alignment, thematic breaks, emphasis / strong / strike / code spans / links (hints and link destinations hidden). |
| Not supported | Theme CSS (`style`, custom `@theme` files), `<style>` / HTML rendering, math rendering, emoji shortcodes, image bitmaps, presenter view with notes, Marp's `fitting header`, `backgroundImage` directives, speaker timer. |

## Editing: the block pass in Marp mode

`RtxDoc_marp(d)` is one bounded read of the first 4 KiB. When the
Markdown lex starts fresh (no checkpoint to resume), it sets
`RTX_MDB_MARP` on the block state; the flag then rides in every
checkpoint like `SOFT` / `UNSURE`, so seed / converge / clip carry it and
a windowed lex in the middle of a large deck knows it without looking
back further than the front matter. With the flag, a thematic break at
container depth 0 is `RTX_MDB_S_SLIDE`, not `RTX_MDB_S_HR`; every other
line classifies exactly as before, so a separator never turns a
paragraph into a setext heading or the other way round (Marp's own
reading, see the table). The lex plants the separator with the grammar's
`hr` rule, so it paints as the rule it is.

An edit in the first 4 KiB of a markup document compares `marp: true`
with what the last lex saw (`RtxDoc.marp`); when it flipped, the
document reparses (every checkpoint's block state is stale). An edit
anywhere else costs nothing new.

**Deck index.** `rtx_deck_for(d)` runs the same block pass (pure, no
lex, no runs) from BOF over at most `RTX_DECK_SCAN_MAX` (8 MiB) and
records per slide: separator offset, body range, title (first heading,
else first text line), build-step count, resolved directives, page
number, background image. A slide boundary resets the block state to
"nothing open", so any slide can later be classified again on its own. It
is a one-entry cache keyed by (document, edit stamp, length, path), not a
file-wide structure the editor maintains: it is rebuilt lazily on the
first query after an edit (260 KiB and 3001 slides index in ~2 ms,
`slides_smoke`; a real deck is tens of KiB). Past 8 MiB the index is a prefix: `slide ?/N+` in the status bar,
and nothing there is guessed. This is the one place a deck reads from
BOF — slide *numbers* need every separator before the caret, exactly as
line numbers need the line index; styling and presenting a slide never
do.

Where it shows:

- **Status bar** (both hosts): `slide 3/12` after the line position.
  A deck over 1 MiB (`RTX_DECK_STATUS_MAX`) is not re-indexed per
  keystroke for it: after an edit the bar shows the last index as
  `slide ~3/12` until something that needs the deck (present, go to
  slide, the batch outline) rebuilds it.
- **Outline**: `rtx_deck_outline` — one entry per slide (separator
  offset, title bytes); `cctext --batch deck.md -c slides` prints it
  (number, line, steps, transition, title).
- **Go to Slide** (`go.slide`, palette / Go menu): opens the jump field on
  `#`; `#N` Enter puts the caret on slide N's first text line. `#N` also
  works when typed into **Go to Line** (`Ctrl-G`).
- The separator line belongs to the slide it opens.

## Presenter model

The presenter (`RtxPresent`, one per process) is host-neutral state:
the slide and step shown, the slide and step a transition leaves, the
transition kind, its start time and duration, a typed slide number.

- `step` counts shown build steps (0 … the slide's `nfrag`). **Next**
  shows the next step, then moves to the next slide at step 0. **Back**
  hides the last step, then moves to the previous slide with all its
  steps shown (as Marp's bespoke navigation does).
- Moving to a slide plays **that slide's** `transition`; moving back
  plays the transition of the slide being left, in reverse direction. A
  build step fades in (200 ms); hiding one is instant.
- Keys (both hosts): Right / Down / PgDn / Space / Enter / `n` next;
  Left / Up / PgUp / Backspace / `p` back; Home / End; digits then Enter
  go to that slide; Esc (or `q`, or Shift-F5 again) leaves. cctext-ui
  also takes a click / wheel down (next) and wheel up (back); the
  terminal takes a click and the wheel.
- Leaving puts the caret on the first text line of the slide shown and
  scrolls the editor there.
- The presenter reads the deck through `rtx_deck_for` on every step, so
  an edit (from another process via Safe, say) is picked up; a slide
  index past the end clamps.

**One slide as lines.** `rtx_slide_view` classifies the slide's bytes
with a fresh block state (Marp mode) and makes `ensure_hl` lex exactly
that range (`rtx_hl_keep`: no clip of the pane's window), then emits one
line per source line — heading (level), paragraph, list item (depth,
marker, task box), code line, table row (cells, header, alignment), rule,
image — each with its build step and quote depth, and its inline content
as styled segments read from the runs (`style_at` / `style_next`, hints
and link destinations skipped: the Rich lens). Front matter, directive
comments, notes and HTML blocks are not lines. The view is bytes plus
offsets; both hosts lay it out their own way.

## Rendering

**cctext-ui** (`gui_present.ccs`) lays a slide out once, in slide pixels
(1280 or 960 × 720), into a display list: text runs (face, size, colour,
build step, line group), filled boxes, frames, rules. Headings are bold
and larger (54 / 44 / 36 / 32 / 30 / 28 px over a 30 px body), code is
the mono face on a dark band with the grammar's scope colours, tables are
measured columns with a tinted header and rules, quotes get a bar, `lead`
/ `uncover` centre the body. Header, footer and page number sit in the
margins. Painting scales the list to the window, letterboxed on a dark
ground; the list is cached per (slide, edit stamp) in two slots (shown,
left), so a transition does no layout per frame.

**cctext** (`cctext_present.ccs`) lays the same lines into a box of cells
sized to the deck's aspect (a cell is about 1:2), centred, with a frame,
header, footer and page number; theme colours map to xterm-256; code rows
are a dark band with scope colours; tables get `│` columns and a `─┼─`
rule. The slide is painted into a cell grid and written as one CUP … EL
per row, so the frame diff sends only rows that changed. The bottom row
names the slide and step.

## Animation model

A transition is `(from slide/step, to slide/step, kind, direction, t0,
duration)`; progress is `(now − t0) / duration` through a cubic ease
in-out. Each host paints both slides from their cached layouts:

| Kind | cctext-ui | cctext |
|---|---|---|
| fade | background colours blended, old text fades out, new fades in | wipe |
| fade-out | old fades to black, then new fades in | wipe |
| slide, push | both slides move by the progress (the new one enters from the right going forward, the left going back) | both move by columns |
| cover | the new slide moves in over the still old one | same, by columns |
| reveal | the old slide moves away over the still new one | same, by columns |
| wipe | the new slide is revealed left to right (a clip) | same, by columns |
| zoom | the new slide grows from 55 % about the centre while fading in | wipe |
| morph | *magic move*: each line (heading, item, code line, table row) whose kind, level and text match a line of the other slide moves and scales from its old box to its new one; unmatched lines cross-fade; backgrounds blend | wipe |
| build step | the step's lines fade in and rise 14 px | appears |

Morph matches by content, first unmatched equal key wins, per line group
(a list item's marker and text, one code line, one table row). An
explicit id (Marp CLI's `view-transition-name` CSS) is not read: there is
no CSS.

**Frame clock, idle.** `rtx_present_wait_ms` is ~16 ms while a transition
runs and −1 otherwise. Both host loops fold it into their existing wait
(the same place the caret blink and Safe debounce go): during a
transition they paint every turn and `rtx_present_tick` ends it on time
(the last frame is the still slide); a still slide wakes nothing. The
caret blink is off while presenting. Measured under Xvfb
(`tests/ui_present_test.py`): a 400 ms push painted 32 frames in 0.7 s
with a 16 ms median gap; afterwards 0 paints, 0 wakeups and 0 CPU ticks in
2 s. In the terminal (`tests/tui_pty_test.py present`) a still slide
writes 0 bytes.

## Tests

- `tests/slides_smoke.ccs` (`@smoke`): block-pass classification in Marp
  mode (separators vs quote / list / fence / HTML / indented code /
  setext, non-Marp unchanged), front matter probe edge cases, the deck
  index over strings (global / local / spot / front-matter / comment
  directives, notes, fragments, `headingDivider`, pages with `hold` /
  `skip`, background image keywords, a prefix cut inside a fence),
  transition and colour parsing, `demo.md` slide views (headings, steps,
  code with scopes, table cells and alignment, quotes, images, setext),
  presenter navigation / steps / transitions / idle wait, `marp: true`
  toggled by an edit, and a 260 KiB deck (past `RTX_HL_FULL_MAX`) whose
  middle slide is lexed as a window.
- `tests/tui_pty_test.py present`: Shift-F5, steps, Left, `N` Enter, a
  push then silence, End / Home, Esc back on the slide, `#N` in the jump
  field, `--batch -c slides`.
- `tests/ui_present_test.py`: cctext-ui under Xvfb — 16:9 letterbox, no
  editor chrome, heading colour, a step adds ink and Left removes it, a
  spot background, frames during a transition and none after, the code
  band, Esc back to the editor. Coarse properties, never exact pixels.

## Limits

- No bitmaps: images are labelled frames (cctext-ui has no image API
  yet; a later opaque-render child from md_view.md would draw them).
- No theme CSS; three built-in palettes. Fonts are the editor's prose /
  mono faces.
- A slide's content taller than the slide is clipped (Marp scales
  `fitting` headers; cctext does not).
- A slide over 256 KiB is cut at that size in the view.
- `headingDivider` splits the index but the lex still paints the heading
  as a heading (it is one).
- The terminal has no fades: fade / zoom / morph are wipes there.

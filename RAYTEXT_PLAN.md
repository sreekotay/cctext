# Raytext plan

Standalone Concurrent-C editor. Compiler repo is a toolchain, not a parent tree.

## Core

- Piece tree: page store (original + spilled add) + red-black order-statistic tree (byte length and `'\n'` weights).
- Sections: layout policy (`code` / `prose`).
- Runs: style intervals inside a section (bold/italic/mono) plus syntax-token kinds.
- Highlight: interned scopes on runs; frontends theme a prefix. Spike lexer or a lowered TextMate table (`@grammar` parses `.tmLanguage.json`). Same run list.
- Layout: `measure(run, bytes) -> width` and `line_height(run) -> height`. Raylib supplies glyph advances; `cctext` supplies `wcwidth`.
- Commands and caret are byte offsets. Stream selection is `[sel_anchor, caret)`. Up/down keep a goal column (or EOL after End). Alt-arrows / Alt-drag is a column box.
- Edits are replaces on a history stack (coalesced typing; large deletes omit inline hist bytes). Save writes pieces to a sibling temp file, preserves mode, fsyncs (file + best-effort dir), then renames. Dirty is `hist.head != saved_head`.
- Offsets are bytes. UTF-8 (grapheme motion / display width) is planned later — keep the byte core; see DESIGN Encoding.
- After an edit, highlight re-lexes the touched section (full markup scan only if the edit involved `=`, `*`, backtick, or a newline). Layout relayouts the touched physical line(s) and shifts the rest.
- A workspace holds several documents and one or two panes.

## Frontends

- `raytext` — Raylib window (`#include <raylib.h>`), GPU quads for the visible layout only. Not `DrawText` on the document.
- `cctext` — POSIX termios + ANSI. SGR mouse (`1000`+`1002`+`1006`): click/drag is `rtx_layout_hit` → caret/selection. Wheel scrolls. Same highlight runs as the GUI.

C FFI defaults to `@blocking`. The UI loop is a blocking main. Layout may run in a nursery.

## Spike success

One file, mixed code + prose, insert/delete through the piece tree, scroll a giant file, caret in mixed text, same file in TTY and Raylib, headless tests for the tree and a fake `WidthMeasure`.

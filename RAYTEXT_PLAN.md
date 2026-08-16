# Raytext plan

Standalone Concurrent-C editor. Compiler repo is a toolchain, not a parent tree.

## Core

- Piece tree: original mmap + add buffer, red-black order-statistic tree (byte length and `'\n'` weights).
- Sections: layout policy (`code` / `prose`).
- Runs: style intervals inside a section (bold/italic/mono) plus syntax-token kinds.
- Highlight: core lexer over `code` sections (C/CC keywords, strings, comments, numbers). Not a frontend concern. Later: `@grammar` or tree-sitter; same run list.
- Layout: `measure(run, bytes) -> width` and `line_height(run) -> height`. Raylib supplies glyph advances; `cctext` supplies `wcwidth`.
- Commands and caret are byte offsets.

## Frontends

- `raytext` — Raylib window (`#include <raylib.h>`), GPU quads for the visible layout only. Not `DrawText` on the document.
- `cctext` — POSIX termios + ANSI. SGR mouse (`1000`+`1006`): click is `rtx_layout_hit` → caret. Wheel scrolls. Same highlight runs as the GUI.

C FFI defaults to `@blocking`. The UI loop is a blocking main. Layout may run in a nursery.

## Spike success

One file, mixed code + prose, insert/delete through the piece tree, scroll a giant file, caret in mixed text, same file in TTY and Raylib, headless tests for the tree and a fake `WidthMeasure`.

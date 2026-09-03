# cctext plan

Standalone Concurrent-C editor. Compiler repo is a toolchain, not a parent tree.

## Core

- Piece tree: page store (original + spilled add) + red-black order-statistic tree (byte length and `'\n'` weights).
- Sections: layout policy (`code` / `prose`).
- Runs: style intervals inside a section (bold/italic/mono) plus syntax-token kinds.
- Highlight: interned scopes on runs; frontends theme a prefix. Spike lexer or a lowered TextMate table (`@grammar` parses `.tmLanguage.json`). Same run list.
- Layout: `measure(run, bytes) -> width` and `line_height(run) -> height`. The GUI supplies glyph advances via Core Text; `cctext` supplies `wcwidth`.
- Commands and caret are byte offsets. Stream selection is `[sel_anchor, caret)`. Up/down keep a goal column (or EOL after End). Alt-arrows / Alt-drag is a column box.
- Edits are replaces on a history stack (coalesced typing; large deletes keep piece refs so undo/redo can splice them back). Save writes pieces to a sibling temp file, preserves mode, fsyncs (file + best-effort dir), then renames. `--backup` writes through the path (keeps symlink / inode) and copies only the dirty span to `path~` (RTXB). Dirty is `hist.head != saved_head`. Save stamps mtime + size + inode and refuses if that identity drifted (`file changed on disk`) unless overwrite is set.
- Offsets are bytes. Text views walk UAX #29 extended grapheme clusters for motion / measure / wrap; hex stays a byte camera. See DESIGN Encoding.
- After an edit, highlight re-lexes the touched section. A full section rescan is only for `=` (or a large delete) — `*` / `` ` `` are style. Layout relayouts the touched physical line(s) and shifts the rest.
- Mark motion (`Ctrl-K/P`) and invalid step (`Ctrl-E/R`) walk current highlight runs only. Fold (`Ctrl-T`) stores an in-window heading region and layout skips its interior. No scan, no AST.
- A workspace holds several documents and one or two panes.

## Frontends

- `cctext-gui` — Cocoa window + Core Text (macOS). Display-list paint into an NSView.
- `cctext` — POSIX termios + ANSI. SGR mouse (`1000`+`1002`+`1006`): click/drag is `rtx_layout_hit` → caret/selection. Wheel scrolls. Same highlight runs as the GUI.

C FFI defaults to `@blocking`. The UI loop is a blocking main. Layout may run in a nursery.

## Spike success

One file, mixed code + prose, insert/delete through the piece tree, scroll a giant file, caret in mixed text, same file in TTY and GUI, headless tests for the tree and a fake `WidthMeasure`.

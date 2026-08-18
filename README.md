# Raytext

A text editor in [Concurrent-C](https://github.com/sreekotay/concurrent-c): one document core, two frontends — **raytext** (Raylib GPU) and **cctext** (POSIX console).

This is a standalone app. It needs `ccc` on `PATH` (or `CCC=`). It does not live inside the compiler repository.

## Why

- **Piece tree** over a page store (path original + spilled add) plus borrowed buffers. Offset and line lookup are O(log pieces).
- **Sections** set layout policy (code vs prose). **Runs** inside a section are rich text and syntax tokens.
- **Syntax highlight** is a core pass over code sections. Runs carry interned scopes; frontends theme a prefix. Spike lexer, or a lowered TextMate table when the path’s extension is in a grammar loaded at runtime (`RTX_GRAMMARS`, else `<exe>/grammars`, else `<exe>/../testdata/grammars`). Open `testdata/samples/` to try them.
- Measure-generic layout so wrap and hit-test are the same algorithm in pixels (Raylib) and cells (`cctext`). View cycles with `Ctrl-L`: default → wrap → hex (offset|hex|ASCII) → default.
- Headless tests do not open a window.

## Status

Piece tree, document, layout, C/CC highlight, save, undo/redo, selection, incremental relayout, and multifile splits. **cctext** and **raytext** share that core.

## Setup

```bash
./setup.sh                  # checks ccc; Raylib is optional for now
./make.shcc @               # list tasks
./make.shcc @smoke          # piece-tree + layout + edit-session + find + large-file
./make.shcc @large          # testdata/generated/large.txt (~3 MiB; LARGE_BYTES= or @large)
make giant                  # testdata/generated/large_8G.txt (~8 GiB; slow)
make giant-json             # testdata/generated/large_2G.json (~2 GiB JSON array; slow)
make giant-smoke            # open that file via page store (mid-line + tiny insert)
make perf                   # ops×sizes + binary/RSS table → testdata/perf/results/…
make perf-record            # that table + refresh testdata/perf/baseline.env pins
make perf-check             # fail if 8G open/scroll/jump/insert regress
./make.shcc @cctext         # console editor (`--release`; DEBUG=1 keeps asserts)
./make.shcc @raytext        # Raylib GUI (needs `brew install raylib`)
./bin/cctext testdata/mixed.txt testdata/small.txt
./bin/cctext --wrap testdata/wrap.txt --hex testdata/mixed.txt
./bin/raytext --view=hex testdata/generated/large.txt
# ./bin/raytext testdata/generated/large_8G.txt
```

**raytext** has a File / Edit / View / Go menu bar (click a title, then an item). The GUI matches **cctext**: blinking caret, idle skip-layout, unlock scroll. **Esc** still opens the key-binding overlay in both frontends. **g** / **Ctrl-G** jumps to a line (or `N%`). **o** / **Ctrl-O** opens a file in the focused view — the GUI uses the system dialog (live, no restart); **cctext** uses the in-app browser. **Ctrl-Shift-O** keeps that browser in the GUI as a reference (glob, walk directories). **l** / **Ctrl-L** cycles default / wrap / hex. **u** / **Ctrl-U** unlocks the pane from the caret; find and jump still land in the active view. Line numbers are in the gutter. Ctrl/Cmd chords work while the overlay is closed. Unsaved quit asks Save / Don't save / Cancel.

Install `ccc` with Homebrew (`brew tap sreekotay/concurrent-c` / `brew install --HEAD …/ccc`) or from a concurrent-c checkout (`PREFIX=$HOME/.local ./cc-install.sh`). TextMate schema parse uses `<ccc/std/json.cch>` / `include JsonKeep` (closed `TmGrammar` stays in-tree). `#include <raylib.h>` needs a lowerer that seeds passthrough C types (0.3.3-139 or last-good after that).

## Layout

```
core/         document — no raylib.h, no termios
frontend/     Raylib GUI and TTY
tests/        headless smokes
testdata/     small fixtures; generated/ is gitignored
```

See [RAYTEXT_PLAN.md](RAYTEXT_PLAN.md).

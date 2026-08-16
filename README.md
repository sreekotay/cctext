# Raytext

A text editor in [Concurrent-C](https://github.com/sreekotay/concurrent-c): one document core, two frontends — **raytext** (Raylib GPU) and **cctext** (POSIX console).

This is a standalone app. It needs `ccc` on `PATH` (or `CCC=`). It does not live inside the compiler repository.

## Why

- **Piece tree** over mmap (original file never moves) plus an add buffer. Offset and line lookup are O(log pieces).
- **Sections** set layout policy (code vs prose). **Runs** inside a section are rich text and syntax tokens.
- **Syntax highlight** is a core pass over code sections (C/CC lexer for the spike). Frontends only map token kind → color.
- **Measure-generic layout** so wrap and hit-test are the same algorithm in pixels (Raylib) and cells (`cctext`).
- Headless tests do not open a window.

## Status

Piece tree, document, layout, C/CC highlight, **cctext** (keyboard + SGR mouse caret), and **raytext** (Raylib) are in.

## Setup

```bash
./setup.sh                  # checks ccc; Raylib is optional for now
make smoke                  # piece-tree + layout/highlight tests
make cctext                 # console editor (click to place caret)
make raytext                # Raylib GUI (needs `brew install raylib`)
./bin/cctext testdata/mixed.txt
./bin/raytext testdata/mixed.txt
```

A `ccc` built from a concurrent-c checkout writes `out/` into that checkout unless `--out-dir` / `--bin-dir` are absolute. `make` passes `$(CURDIR)/out` and `$(CURDIR)/bin`. You can also set `CC_OUT_DIR` / `CC_BIN_DIR`.

Sources omit the line-1 `#!ccc` header: stripping it copies the TU into a cache dir, and quoted `#include` of project `.cch` files then miss the source tree. Kind comes from the suffix.

Install `ccc` with Homebrew (`brew tap sreekotay/concurrent-c` / `brew install --HEAD …/ccc`) or from a concurrent-c checkout (`PREFIX=$HOME/.local ./cc-install.sh`). `#include <raylib.h>` needs a lowerer that seeds passthrough C types (0.3.3-139 or last-good after that).

## Layout

```
core/         document — no raylib.h, no termios
frontend/     Raylib GUI and TTY
tests/        headless smokes
testdata/     small fixtures; generated/ is gitignored
```

See [RAYTEXT_PLAN.md](RAYTEXT_PLAN.md).

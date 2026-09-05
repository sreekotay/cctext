# raytext

[![build](img/build.svg)](https://example.com/ci) [![license](img/license.svg)](LICENSE)

A text editor where **bytes stay the truth**. The *Rich pane* is a lens:
delimiters like `**` and `` ` `` become zero-width atoms, content keeps its
style, and [links](https://example.com) fold to their text. Nothing is
stored twice; there is no Markdown to HTML step and no second document.

## Install

```sh
git clone https://example.com/raytext.git
cd raytext && ./make.shcc
```

Run `cctext FILE` in a terminal, or `cctext-gui FILE` for the windowed host.
Both share one core: input, pump, layout, paint.

## Usage

Open a file and move with the arrow keys. The caret is a *byte offset*; a
hidden hint is one step to cross, and backspace on a hint is **unwrap**.

- `l` cycles default, wrap, hex, and grid views
- `Tab` inside a table jumps to the next cell
- `Ctrl-Z` undoes bytes, never lens state

Selection is a byte stream too. A cut that reaches one hint of a pair
reaches both, so there is no broken-markup case to repair afterwards.

## Features

| Feature | Status | Notes |
|---------|:------:|------:|
| Fence highlight | **done** | `c`, `json`, `sh` |
| Pipe tables | *partial* | cells wrap |
| Folding | planned | [issue](https://example.com/1) |
| Opaque children | planned | mermaid, images |

## Example

```c
#include <stdio.h>
int main(void) {
    printf("hello\n");
    return 0;
}
```

The fence body above is highlighted as C. Its `**` and `#` bytes, if any,
are never styled as Markdown.

## Roadmap

- [x] Inline marks as atoms
- [x] Fence injection
- [ ] Table cell children
- [ ] Heading folds
  - [ ] fold to EOF
  - [ ] fold nested levels

## Notes

> Marks are clusters with one more join rule.
> A pair's two hints are **one atom in two places**.

<details>
<summary>Why not an AST?</summary>

Bytes are the document. A lens is layout-epoch scratch, like `RtxGridGeom`.
Reclassify on the next fill; never store a tree.

</details>

---

## License

MIT. See `LICENSE` for details. Contributions follow the same terms.
Footer text runs to end of file; folding this heading hides through EOF.

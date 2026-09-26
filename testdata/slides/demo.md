---
marp: true
theme: default
size: 16:9
paginate: true
header: cctext slides
footer: testdata/slides/demo.md
transition: fade
---

<!-- _class: lead -->
<!-- _paginate: false -->

# cctext presents

Marp decks, edited as bytes and shown as slides

`Shift-F5` to present, `Esc` to come back

---

<!-- transition: slide -->

## Build steps

A fragmented list (`*`) appears one item at a time:

* First, the bytes
* Then the block pass
* Then the Rich lens
* And finally, slides

---

## Ordered builds

A `1)` list is fragmented too; a `1.` list is not:

1) Split on `---`
2) Read the directives
3) Present

1. always
2. visible

---

<!-- transition: push 400ms -->

## Code keeps its colours

```c
/* a --- inside a fence is not a slide break */
int main(void) {
    const char *sep = "---";
    return sep[0] == '-';
}
```

---

<!-- _backgroundColor: #1e1e2e -->
<!-- _color: #cdd6f4 -->
<!-- transition: cover -->

## Tables and quotes

| Transition | Kind    | Drawn as        |
|:-----------|:-------:|----------------:|
| fade       | blend   | cross-fade      |
| slide      | motion  | both move       |
| push       | motion  | pushed out      |

> Bytes are truth: nothing here is stored twice.

---

<!-- transition: reveal -->

![bg left:40% fit](images/marp.png)

## Split background

The image fills the left 40 %; the text keeps the right.

- `bg left` / `bg right`
- `fit`, `contain`, `cover`, `auto`

---

<!-- _class: invert -->
<!-- transition: wipe -->

## Inverted class

**Bold**, *italic*, ~~strike~~, `code` and a [link](https://marp.app)

![w:320](images/diagram.png)

---

<!-- transition: zoom 500ms -->

## Zoom

<!--
This comment is not directives: Marp reads it as a presenter note,
so it is not drawn.
-->

Growing from the centre.

---

<!-- transition: morph -->

## Magic move

- Alpha
- Beta

---

## Magic move

Matched lines glide to their new place:

- Beta
- Alpha
- Gamma

---

<!-- _paginate: skip -->
<!-- transition: fade-out -->

# Thanks

Setext title
------------

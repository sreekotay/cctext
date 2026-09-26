---
title: Fixture
count: 2
---

Setext level one
================

Setext *level* two
------------------

#hashtag is not a heading

~~~python
def f():
    return "~~~"
~~~

````md
```
inner fence is body
```
````

1. item with a fence:

   ```c
   int x = 1;
   ```
2. [ ] task in an ordered list

> quote *em
> spans* lines
lazy continuation

    indented **code**

* * *

```
| not | a table |
|-----|---------|
```

$$
E = mc^2
$$

<details>
<summary>more</summary>

Body with **bold**.

</details>

Text with _em_, __strong__, ~~strike~~, ``a`b``, $x^2$, \*literal\*, &amp;.
Links: [a [b] c](u(v)), ![alt](img.png), [x][ref], [1] and [docs](url), [^1], [[Wiki|label]].
Autolinks: <https://a.b/c>, <me@x.org>, www.example.com.
Hard break with two spaces  
next line.

[ref]: https://example.com "Title"
[^1]: The note.

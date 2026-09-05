# Fences

```c
int main(void) { return 0; }
```

```json
{"k": "v", "n": 1}
```

```python
def f(x):
    return x * 2
```

```js
const f = (x) => x * 2;
```

```html
<div class="a">hi</div>
```

```sh
echo "$HOME" | wc -c
```

```yaml
key: value
list:
  - one
```

```css
a { color: red; }
```

```foo
unknown language stays raw
```

```
no info string
```

~~~
tilde fence body
~~~

````md
```
inner triple fence stays inside
```
````

```
**not bold** and # not a heading
- not a list
```

Indented code block:

    four spaces of indent
    **not bold** either

Unterminated fence follows:

```c
int open = 1;
/* no closing fence */

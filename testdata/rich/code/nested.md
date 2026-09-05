# nested.md: depth-2 embeds

Each fence below embeds a language that itself has an embedded span.

## Python fence containing a docstring

```python
def greet(name):
    """Return a greeting.

    Contains a fake fence marker ``` and the word def.
    """
    return f"hi {name}"
```

## HTML fence containing a script

```html
<script>
  const s = "</scr" + "ipt>";
  console.log(`template ${s}`);
</script>
```

## JS fence containing a template literal

```js
const doc = `
# not a markdown heading, this is inside a template
\`\`\`sh
echo "not a fence either"
\`\`\`
`;
```

## Shell fence containing a heredoc

```sh
python3 - <<PY
def main():
    """docstring, two levels down"""
PY
```

## Fence inside a blockquote

> Quoted intro line.
>
> ```python
> def quoted():
>     """docstring in a quoted fence"""
> ```
>
> Quoted outro line.

## Fence inside a list item

1. First item.
2. Second item with code:

   ```sh
   cat <<EOF
   heredoc in a list-item fence
   EOF
   ```

3. Third item.

Trailing prose after all fences.

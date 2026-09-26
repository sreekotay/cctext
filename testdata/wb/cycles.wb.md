# Loop

Every member of a cycle is `#cycle(path)`; what reads a cycle gets that
error; the rest of the workbook still calculates.

```calc
a  = b + 1
b  = c + 1
c  = a + 1
d  = a * 2
e  = e + 1
ok = 5
```

Table: C

| x | y          | z           |
|---|------------|-------------|
| 1 | `=@y`      | `=sum(C.z)` |
| 2 | `=@x * 10` | `=@y + ok`  |

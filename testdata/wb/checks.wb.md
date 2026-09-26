# Checks

Typed columns (int, dec, text, date), error values and their
provenance, and the scalar language.

Table: T

| a  | b    | c | d          | f          |
|----|------|---|------------|------------|
| 10 | 2.5  | x | 2026-01-31 | `=@a / @b` |
| 7  | 0    | y | 2026-02-28 | `=@a / @b` |
|    | 1.25 | z | 2026-03-01 | `=@a + @b` |
| 3  | 4    | w | 2026-12-31 | `=@c + 1`  |

```calc
s_a     = sum(T.a)
s_b     = sum(T.b)
s_f     = sum(T.f)
s_f_ok  = sum?(T.f)
n_a     = count(T.a)
n_t     = count(T)
m       = max(T.d)
days    = max(T.d) - min(T.d)
bad     = nope + 1
badcol  = T.zz
colval  = T.a
dec     = 1.10 * 3
decmul  = 1.5 * 1.5
mixed   = 1 + 0.5
fl      = 1e3 / 8
txt     = "a" & 1 & true
cmp     = "abc" < "abd"
iff     = if s_a > 10 then "big" else "small"
iff2    = if(s_a > 100, 1, 2)
co      = coalesce(null, 5)
rnd     = round(2.345, 2)
rnd0    = round(2.5)
fl2     = floor(-2.5)
ce      = ceil(2.1)
ab      = abs(-4.25)
ln      = len("héllo")
up      = upper("abc")
dateadd = 2026-01-31 + 30d
iserr   = iserror(1 / 0)
iferr   = iferror(1 / 0, -1)
cmpbad  = "a" < 1
mod     = 17 % 5
neg     = -(3)
nulladd = null + 1
syntax  = (1 + 
view    = group T by c { n: count() }
```

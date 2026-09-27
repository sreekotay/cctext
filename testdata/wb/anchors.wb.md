# Anchors

Anchors name fixed rows: `{#name}` at the start of a row's first cell
(not part of its value). `#first` and `#last` are implicit. Bare
positions (`T.amount[5]`) are an error: rows move, anchors move with
them.

Table: T

| k        | amount | run                              | prev         |
|----------|--------|----------------------------------|--------------|
| {#q1} a  | 10     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| b        | 20     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| {#q2} c  | 30     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| d        | 40     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| {#q3} e  | 50     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| {#q3} f  | 60     | =coalesce(@run[-1], 0) + @amount | =@amount[-1] |
| {#tail}  | 5      | =T#q1.amount + @amount           | =@k          |

```calc
at_q1      = T#q1.amount
after_q2   = T#q2[+1].amount
first_k    = T#first.k
before_end = T#last[-1].amount
q1_to_q2   = sum(T[#q1 .. #q2].amount)
reversed   = sum(T[#q2 .. #q1].amount)
dup        = T#q3.amount
missing    = T#nope.amount
position   = T.amount[5]
all_rows   = count(T[#first .. #last])
off_top    = T#q1[-1].amount
top_run    = max(T[#q1 .. #last].run)
no_end     = sum(T[#q1 .. #gone].amount)
dup_end    = avg(T[#q2 .. #q3].amount)
tail_run   = T#tail.run
too_far    = T#q1[+65].amount
```

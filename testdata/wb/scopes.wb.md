Params are global; calc names belong to their sheet (H1) and are
qualified from another one (`North.total`).

```params
rate = 0.5
```

# North

```calc
total  = sum(Orders.qty)
scaled = total * rate
```

Table: Orders

| id | qty | unit_price | line                  |
|----|-----|------------|-----------------------|
| 1  | 3   | 2.00       | `=@qty * @unit_price` |
| 2  | 5   | 1.50       | `=Orders[@qty] * 1`   |
| 3  | 2   | 4.00       | `=@qty * @unit_price` |

# South

Table: `Price List`

| item | `list price` |
|------|--------------|
| a    | 10           |
| b    | 12.5         |

```calc
total   = 7
both    = total + North.total
dupe    = first(Orders[qty > 2]).id
many    = Orders[qty > 2].id
none    = Orders[qty > 100].id
lp      = `Price List`[item = "b"].`list price`
Dupname = 1
dupname = 2
```

# Revenue

A workbook is Markdown: sheets are H1 sections, tables are GFM tables
under a `Table: Name` caption, and names live in `params` / `calc`
fences. Values are never written into this file.

```params
asof   = 2026-09-22
fx_eur = 1.0000
```

Table: Sales

| order_id | date       | region | cust | amount  | cost   | margin               |
|----------|------------|--------|------|---------|--------|----------------------|
| 1        | 2026-09-01 | EU     | 10   | 120.00  | 80.00  | `=@amount - @cost`   |
| 2        | 2026-07-15 | US     | 11   | 300.50  | 210.25 | `=@amount - @cost`   |
| 3        | 2026-09-20 | EU     | 12   | 99.99   | 50.00  | `=@amount - @cost`   |
| 4        | 2026-05-02 | APAC   | 10   | 1000.00 | 700.00 | `=@amount - @cost`   |
| 5        | 2026-09-21 | US     | 13   | 45.10   | 20.10  | `=@amount - @cost`   |

Table: Targets

| region | target | actual                                      | gap                  |
|--------|--------|---------------------------------------------|----------------------|
| EU     | 200    | `=sum(Sales.amount where region = @region)` | `=@actual - @target` |
| US     | 400    | `=sum(Sales.amount where region = @region)` | `=@actual - @target` |
| APAC   | 900    | =sum(Sales.amount where region = @region)   | =@actual - @target   |

```calc
eu_total   = Targets[region = "EU"].actual
recent_mgn = sum(Sales.margin where date >= asof - 90d)
n_orders   = count(Sales)
eu_orders  = count(Sales where region = "EU")
avg_amt    = avg(Sales.amount)
max_amt    = max(Sales.amount)
min_date   = min(Sales.date)
regions    = distinct(Sales.region)
any_big    = any(Sales.amount > 500)
all_cost   = all(Sales.cost > 0)
eu_eur     = eu_total * fx_eur      # dec scales add: 2 + 4
label      = "EU: " & eu_total
```

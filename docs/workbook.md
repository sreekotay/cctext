# Workbooks: named tables, dependency recalc, giant data

**Status: W0 (Names) and W2 (Aggregates, embedded tables) landed** — see [W0 as built](#w0-as-built): embedded named tables, `params` / `calc`, the formula language, the DAG with Tarjan and cutoff recalc on edit deltas, values in the Rich lens, rename; and [W2 as built](#w2-as-built): maintained aggregates updated by row deltas with exact float / dec sums, `group`, rows inserted and deleted as deltas, and the live value annotation while the caret is in a formula. W1 and W3–W5 are design. Open questions in §9 (decisions for W0 there).

TODO.md: *"spreadsheets / cell references — NOT normal reference — named,
and dependencies based."* This note answers that. A **workbook** is a
text file of **named tables** with **typed, named columns** and
**formulas that reference tables, columns and rows by name**. Recalc walks
a **dependency graph** over those names and pushes each edit **only to
dependents, in topological order**. Tables may be external CSV / TSV
files of many GB. Aggregates and lookups over them are **maintained**,
not recomputed. No A1 cells, no volatile functions, no rescan per recalc.

Same lens as [md_view.md](md_view.md): **bytes are truth**. The workbook
is a Markdown file, and every value, index and summary is derived state
that can be rebuilt from bytes. The one new law is stated as an exception
in [Locks](#locks).

## 0. What was rejected, and why this is different

One earlier proposal computed `sum(...)` by streaming 2 MiB byte blocks,
the way find does (`core/find.ccs` `rtx_find_run`). The owner rejected
it: *"you'd have to follow the workbook/table — 2MB scan doesn't work."*
Byte blocks do not know records, schemas or dependents, and every recalc
pays for the whole file.

This design reads the file **once per identity** to **build a table
index**. The index is persisted and then kept current by **edit deltas**.
A recalc never visits a byte its edit did not touch, apart from at most
one leaf (≤ 64 KiB) on each side. The only block loop that remains is
the one-time progressive **build**, and it produces a record-aligned
structure. It does not produce an answer.

## 1. Model

```
Workbook (one .wb.md file, one RtxDoc)
 ├─ params        asof = 2026-09-22         (explicit, no NOW())
 ├─ uses          prior = "../q2.wb.md"     (read-only import, namespaced)
 ├─ Sheet "Revenue"  (H1 section = tab)
 │   ├─ Table Sales      external  data/sales.csv   (RtxDoc of its own)
 │   ├─ Table Targets    embedded  GFM table in this file
 │   └─ calc names       eu_total, growth, …
 └─ Sheet "Customers" …
Table = name + source + parser config + schema (ordered, typed columns)
      + computed columns (virtual) + key / index declarations
```

**Names and scope.** Table names are global to the workbook. Every
table therefore appears once in the dependency graph, no matter which
sheet shows it. Scalar names (`calc`) are scoped to their sheet. From
another sheet they are qualified: `Revenue.eu_total`. Imports are
namespaced (`prior.eu_total`) and read-only. Column names are scoped to
their table. Inside a row context, `@col` means the current table's
column. The grammar is `[A-Za-z_][A-Za-z0-9_]*`, and a backtick form
`` `Amount (EUR)` `` allows arbitrary text. Names are case-sensitive, and
names that differ only in case are an error at bind time.

**Bindings are workbook-owned.** A column's *name* lives in the
workbook, and its *header text* is a binding into the source. If you
rename `Sales.amount` in the workbook, the CSV header does not change.
Rename is a refactor over the workbook doc. Every reference site is a
byte span that the binder already knows, because the graph edges carry
spans. The rewrite is **one command, one undo step**: one `replace` per site,
applied in descending offset order under one hist group (DESIGN
[Edit groups](../DESIGN.md#edit-groups)). A rename that also touches
the external file's header is a workspace transaction across both docs. If a
header in the external file changes (for example `Amount` becomes
`Amt`), the binding breaks. The result is a `#schema` error on the
column, never a silent rebind.

**Truth vs derived.**

| State | Kind | Lives | Rebuilt from |
|---|---|---|---|
| Workbook text, external CSV bytes | truth | files, piece tree (`core/piece_tree.cch`) | — |
| Parsed workbook: names, schemas, formulas, graph | derived | workbook epoch (arena on the workbook `RtxDoc`) | full parse of the workbook doc (small; capped `RTX_WB_MAX`, 8 MiB) |
| Table index: leaves, counts, zone maps, aggregate partials, key indexes | derived, persisted | index epoch on the *table's* `RtxDoc` + cache dir (`RTXI`) | one progressive build over the table bytes |
| Formula values | derived, not persisted in the file | workbook epoch; cached in `RTXV` beside `RTXI` | graph eval over index roots |
| Views (filter / sort) | derived | layout / view epoch | index + predicate |

Values are **never written into the workbook**. This is md_view's
"Derived values: layout-epoch, read-only" lock, extended so that values
persist to the cache and do not reach the file. `export` (`--batch`
verb) is the only path that writes values out, and it writes to a
different file.

## 2. File format

The workbook is **Markdown** (`*.wb.md`). It renders on GitHub, diffs
line by line, and uses the lens the editor already has. Sheets are
H1 sections. Tables and formulas are fences with an info string (the
injection mechanism in md_view, wedge 4) or GFM tables (MD table child,
`core/md_table.cch`).

````markdown
# Revenue

```params
asof   = 2026-09-22
fx_eur = 1.0000
```

```table Sales
source:  data/sales_2026.csv        # path relative to this file; never copied
format:  csv; header: 1; quote: '"'; delim: ','
key:     order_id
index:   cust                       # secondary key index (lookups / joins)
columns:
  order_id: int        <- "Order ID"
  date:     date       <- "Date"
  region:   enum       <- "Region"
  cust:     int        <- @4        # bind by position when there is no header
  amount:   dec(2)     <- "Amount (EUR)"
  cost:     dec(2)     <- "Cost"
computed:
  margin:   dec(2) = @amount - @cost
  tier:     enum   = Customers[id = @cust].tier
```

Table: Targets

| region | target   | actual                                      | gap                  |
|--------|----------|---------------------------------------------|----------------------|
| EU     | 1200000  | `=sum(Sales.amount where region = @region)` | `=@actual - @target` |
| US     | 2000000  | `=sum(Sales.amount where region = @region)` | `=@actual - @target` |

```calc
eu_total  = Targets[region = "EU"].actual
gold_mgn  = sum(Sales.margin where tier = "gold" and date >= asof - 90d)
by_region = group Sales by region { n: count(), total: sum(amount), top: max(amount) }
growth    = eu_total / prior.eu_total - 1
```
````

- **External table** (a ```` ```table Name ```` fence). The `source`
  file opens as its own `RtxDoc` through `from_path` (page store, no
  copy). `<-` binds a column to header text or to a position `@k`.
  Header text is checked against line 0 of the source at open.
- **Embedded table** (a `Table: Name` line directly above a GFM table).
  Headers are column names. Types are inferred per column (int ⊂ dec ⊂
  float ⊂ text) unless a `types:` line follows. Cap: 10k rows or 4 MiB.
  An `apply` named `externalize` moves the body to `Name.csv` and writes
  a `table` fence in one `replace`.
- **Formulas live in three places.** They can be `calc` names, schema
  `computed:` columns (virtual: never in bytes, painted as extra grid
  columns), or embedded cells whose content starts with `=` (in Rich the
  value is painted and the formula is the hint, revealed on entry, the
  md_view "Derived value" child). A CSV cell starting with `=` is text.
  External files never contain formulas.
- **Grammar**: `wb.tmLanguage.json` declares `kind: markup` and injects
  `source.wbcalc` into `params` / `calc` / `table` fences
  (`rtx_tm_rt_for_info`). Paint, nav and apply all come from the
  existing machinery.

## 3. Formula language

Formulas are expressions over **tables, columns, rows and scalars**.
The language has no cell addresses.

| Form | Meaning |
|---|---|
| `Sales` | table (relation) |
| `Sales.amount` | column (multiset of typed values) |
| `@amount`, `Sales[@amount]` | row context: this row's value (computed columns, embedded cells) |
| `Sales where region = "EU" and date >= asof - 30d` | filtered relation |
| `Customers[id = @cust]` | keyed lookup, 0 or 1 row. More than 1 is `#dup` unless `first(...)` |
| `sum / count / avg / min / max / distinct / any / all (col [where p])` | aggregates |
| `group T by k { name: agg, … }` | derived table (small result) |
| `join Sales, Customers on cust = id` | derived relation (a view; materialized only when bounded) |
| `sort T by k desc`, `top 10 T by amount` | views |
| `if c then a else b`, `coalesce`, arithmetic, `&` concat, date ± `Nd` | scalar ops |

**Types.** `int` (i64), `dec(p)` (scaled i64 with i128 accumulators),
`float` (f64), `text`, `bool`, `date`, `datetime`, `enum` (interned per
column), `table`, `error`. Coercion goes only upward
(int → dec → float). Comparisons across types are `#type`. Parsing a
typed cell yields either a value or an error.

**Errors are values.** They carry their provenance: `#parse(Sales row
81,233,017 amount "12,5")`, `#div0`, `#ref(Sales.amt)`,
`#schema(Amount)`, `#cycle(a→b→a)`, `#dup`, `#type`. An aggregate over a
column with parse errors returns an error by default. `sum?(…)` skips
errors and reports the count it skipped. Every column summary tracks
`bad` so the count is O(1).

**Determinism.** The language has no volatile functions. `now`,
`today` and `rand` do not exist. Time is the `asof` param in the file.
Results depend only on bytes. `dec` sums are exact integers. `float`
sums use an exact expansion (Shewchuk non-overlapping partials, usually
2–3 doubles) per leaf, and those combine exactly up the tree and round
once. The result is **bit-identical no matter the edit history or tree
shape**. That property is what makes subtract-old / add-new exact
(section 4). (As built in W2: a fixed-point superaccumulator rather than
an expansion — the same exactness with a bounded, order-free state; see
[W2 as built](#w2-as-built).)

## 4. Dependency graph and incremental recalc

**Nodes.**

| Node | Example | Maintained as |
|---|---|---|
| Source | `Sales` (the bytes) | table index |
| Column | `Sales.amount` | parse fn plus zone maps in leaves |
| Computed column | `Sales.margin` | per-row fn, evaluated inside leaf partials; never stored |
| **Materialized aggregate (MA)** | `sum(Sales.amount where region=@region)` | a partial per leaf, combined up the index tree; the root is the value |
| Group MA | `group Sales by region {…}` | per-leaf key→partial maps, merged up the tree |
| Lookup | `Customers[id=@cust]` | key index |
| Scalar / cell | `eu_total`, a `Targets` cell | value |
| View | `Sales where …` shown in grid | counted selection tree (section 6) |

Edges come from the parse. They are **static**: no `INDIRECT`, no
names computed at runtime. The graph is therefore known at bind time.
Tarjan SCC runs at bind, and every member of a cycle becomes
`#cycle(path)`. The language has no iterative calc. The topological
order is recomputed only when the *workbook* changes, because a graph
of a few thousand nodes costs microseconds.

**Granularity.** Edges run table → column → MA → scalar. The
**change unit is a leaf delta**: *(table, leaf id, old partials, new
partials)*. Scalars and cells are single nodes. Row-context formulas in
an embedded table are one node per cell, because those tables are small.

**Propagation.** Each change goes through these steps:

1. A table doc takes `replace(lo, n, bytes)`. The index hook
   (section 5) re-splits the touched leaves, typically one. For each MA
   over that table, it recomputes those leaves' partials from the new
   bytes and walks the path to the root. **Invertible** aggregates
   (`sum`, `count`, `avg` = sum/count, `sumsq`/variance) update the
   ancestors by `+new − old`. **Non-invertible** ones (`min`, `max`,
   `distinct`-count via per-leaf HLL, and `first`) recompute each
   ancestor from its children's partials. Fanout is 64 and depth ≤ 4
   for 1e9 rows, so that is at most ~256 partial merges.
2. Each changed MA root marks its dependents dirty. Recalc runs
   dirty nodes in topological order with **early cutoff**: if a node's
   new value equals its old one, its dependents are not visited.
3. Dirty cells and names repaint through the normal layout patch path.
   The workbook doc's value paint is a layout-epoch child, so a changed
   value relays that row (`RtxLayoutPatch`) and does not refill.

**Worked example.** `sum(Sales.amount where region="EU")` over a 10 GB
`Sales`. A user edits one row's `amount` from 120.00 to 150.00:

- The hook finds the leaf by byte descent, O(log n). It re-parses that
  leaf (≤ 64 KiB, already in the page cache because the edit just
  touched it).
- The **fast path** applies when the edit stays inside one record and
  does not cross a record or quote boundary. The hook parses the old
  record, captured pre-replace (bounded by `RTX_GRID_REC_CAP`), and the
  new record. The row delta `(region=EU, 120.00) → (EU, 150.00)` gives
  `Δ = +30.00` on that leaf's partial, and every ancestor adds 30.00.
- If `region` changed from `EU` to `US`, the EU partial moves by
  −120.00 and the US partial (the same MA, a different `@region` binding
  in `Targets`) by +120.00. Both cells are marked dirty.
- Total cost: one record parse, one to four partial updates, and ≤ 4
  tree levels. **No other byte of the 10 GB is read.**

**Where-filters** fall into three cases:

- Equality on an `enum` or indexed column, the same predicate with
  different constants (`where region = @region` across `Targets` rows):
  these compile to **one group MA keyed by region**. All `Targets` rows
  read their own key from it.
- Range predicates (`date >= asof - 90d`) and anything else get an MA
  of their own. The initial build uses **zone maps** (per-leaf min/max)
  to skip leaves that cannot match or to take a leaf's whole partial
  when every row matches.
- Changing `asof` changes the predicate itself. That is a rebuild of
  that MA only, and zone maps prune it. A range over a sorted or
  clustered column touches only the boundary leaves.

**Row-dependent on scalar.** An example is `sum(Sales.amount * fx_eur)`.
The binder rewrites linear forms as `fx_eur * sum(Sales.amount)`, so the
MA does not depend on `fx_eur`. A non-linear dependence
(`sum(max(@amount - threshold, 0))`) makes `threshold` a **full-MA
dependency**. It is marked in the formula bar as `⟳ table-wide` and
rebuilds progressively.

**Cross-table.** `sum(Sales.margin where tier = "gold")`, where `tier`
is a lookup through `Customers`: a change to `Customers[id=42].tier`
uses the **key index on `Sales.cust`** (key → leaf ids). Only the leaves
that hold `cust=42` recompute. Lookups against an unindexed column are a
bind-time error that suggests `index: cust`. They are never a scan.

## 5. Indexing giant tables

The deliberate exception: a table **declared in a workbook** gets a
whole-table record index. A plain `.csv` opened in grid view keeps
today's windowed behavior (`core/grid.cch`: window-lex records, sticky
line 0, `RTX_GRID_REC_CAP` leftover, `+N`/`-L` gutter).

**Structure: a counted B+tree of record-aligned leaves.**

```
leaf  (≈64 KiB of bytes, = RTX_PAGE_SIZE; always starts at a record start)
  nbytes u32, nrec u32, flags (quote state at end = 0, bad rows)
  [per summarized column]  zone: min, max, bad, nulls        (≈24 B)
  [per MA]                 partial (sum/count/exp/min/max/HLL…)
inner (fanout 64)
  Σ nbytes, Σ nrec over children  + merged zone + merged MA partials
```

- **Lengths, not offsets.** An insert or delete changes one leaf's
  `nbytes`/`nrec` and its ancestors' sums. No offset shifts anywhere.
  This is the `RtxNode.subtree_len` / `subtree_lf` augmentation from
  `core/piece_tree.cch`, one level coarser. It is **not** a flat array
  and not a Fenwick tree, because leaf splits need insertion.
- **row → byte**: descend by `Σ nrec` (O(log n)), then split ≤ 64 KiB.
  That is one page read (`RTX_PAGE_SIZE`). **byte → row** is the same
  descent by `Σ nbytes`.
- **Memory math, 1e9 rows × 100 B (100 GB)**: 1.6 M leaves. The
  positional part is 8 B/leaf, about **13 MB**. Each summarized column
  is 24 B/leaf, about **38 MB/column**. Only columns that formulas or
  views reference get summaries. A full offset array would be
  **8 GB** (5 GB packed) and would shift on every insert. For a 10 GB
  table: 160 k leaves, about **1.3 MB** positional and **3.8 MB/column**.
  Inner nodes add 1/64.
- **Leaf maintenance under `replace`.** The hook runs on the table
  `RtxDoc` beside `note_insert`/`note_erase`
  (`core/piece_tree_lines.cch`). It finds the leaves overlapping
  `[lo, lo+n)`, re-parses from the first leaf's start through the end
  of the last, and emits new leaves. It splits above 128 KiB and merges
  below 16 KiB. **Quote re-sync**: if a re-parsed leaf ends in a
  different quote state than before (an unbalanced `"` was typed), the
  next leaves re-parse until their end states converge again. After
  `RTX_WB_RESYNC_MAX` (8 MiB) without converging, the table is
  **damaged from byte X**. The status line says so, and dependents get
  `#parse(…from row N)`, an honest leftover rather than a wrong answer.
  Undo is the same hook in reverse.
- **Record parser** is RFC 4180 with the declared `quote`/`delim`. For
  workbook tables, **grid view uses the index's record boundaries**, not
  the window-lex STRING face, so the two cannot disagree. Plain grid
  keeps `rtx_layout_grid_held`.
- **Key / secondary index** (`key:`, `index:`): key → sorted leaf-id
  list with per-leaf counts. `key:` also enforces uniqueness (`#dup`).
  For a high-cardinality key over 1e9 rows the index is large (about
  8–12 B/row, 8–12 GB). It is **on-disk**: an LSM of sorted runs in the
  cache dir with a RAM delta, loaded page-wise. It is declared only when
  asked for, and the index is **opt-in per column**.
- **Columnar sidecar (phase 5, optional)**: per summarized column, a
  packed typed vector (`dec` as i64, `enum` as u16 ids) per leaf. A
  *new* formula over an already-indexed table then builds its MA from
  the sidecar at memory bandwidth instead of re-parsing CSV. The sidecar
  is invalidated per leaf by the same hook.

**Progressive build.** The build is a Scan-table row (DESIGN §Scan):

| | start | one step | live | resume | deny |
|---|---|---|---|---|---|
| wbidx | `wb_idx_kick` (table bound, index missing or stale) | dest-live wrapper; `@parallel wait` over 2 MiB chunks, `@stage` in file order emits leaves | `idx.h.live()` | `built_off` | refuse above `RTX_WB_TABLES` live builds; one per table |

- **Parallel CSV with quotes.** Each worker parses its chunk
  speculatively under both start states (outside or inside quotes). The
  in-order `@stage` knows the true state from the previous chunk and
  keeps the right parse. This is the standard two-state trick. Leaves
  cut at record boundaries at stage time. The ticket-local scratch
  follows find's lane arena (`RtxFindScratch`).
- MA partials for every MA currently bound are computed in the same
  pass. The same goes for zone maps. The file is read once.
- **Honest partial results.** Until `built_off == len`, an MA's value
  is its root over the built prefix. It is shown as `Σ 812,400.00 · 43%`
  in the pending face. Dependents carry a **pending bit** that
  propagates like an error but keeps a value. Pending values are never
  persisted to `RTXV` or exported as final. `--batch` has `await
  wbidx`, like `await index|island`.
- Budget: ≥ 1 GB/s aggregate on 8 cores for typed CSV parsing, so a
  10 GB first open takes about 10 s in the background. The editor paints
  at once: open stays O(1) (README Perf `open` 0.005 ms) because the
  grid window does not wait on the index.

**Persistence (Safe-style).** The index lives at
`$XDG_CACHE_HOME/cctext/wb/<fnv(realpath)>.i` (`RTX_SAFE_HOME`
overrides, as in `core/safe.cch`). Magic `RTXI`, `ver`, `ver_min`, and
the rule that an unknown version is ignored are all the same as Safe.

- Header: **identity triple** (mtime, size, inode), the same stamp
  save uses; a `schema_hash` over column types and bindings; a
  `parser_hash` over quote, delim and header; and the leaf size.
- Body: the leaf array (positions + zones), MA partials keyed by
  `formula_hash`, and key index runs in sibling files. Inner nodes are
  rebuilt on load, O(leaves), which is about 2 ms for 160 k.
- **Validate on open.** A mismatched triple means the index is tossed
  and rebuilt. As a guard against same-mtime rewrites, the loader also
  hashes the first, last and 8 sampled leaves (64 KiB reads) and
  compares. **Append fast path**: when the size grew, the triple
  otherwise differs, and the last leaf's hash still matches, the index
  keeps its leaves and builds only the tail (log files).
- If the schema or parser hash differs, the positions are kept only
  when `parser_hash` matches, and zones and MAs are rebuilt. An MA whose
  `formula_hash` is new builds alone (from the sidecar when present).
- **Unsaved edits.** The in-memory index follows the piece tree.
  Recovered Safe hist (`RTXS`) replays as `replace`s through the hook,
  so an index persisted for the on-disk identity plus the journal gives
  the edited state without a rebuild. After save, the index is
  re-stamped with the new identity (the hook kept it current).
  Park/evict flushes `RTXI` with the journal.
- **External change while open** (the triple drifts at save or on
  focus): the same "file changed on disk" path as save. The index is
  marked stale, values go pending, and the user reopens.

## 6. Views and UI

- **Filter view** (`Sales where …` opened in grid): a *second counted
  tree* over the same leaves. It stores per-leaf **match counts**
  instead of `nrec`. Row k of the view descends by match count, then
  scans the leaf. Maintenance uses the same leaf hook. Scrollbar and
  `g row N` are exact.
- **Sort view**: an order-statistic B-tree keyed by
  `(sort key, leaf id, ordinal)`. Leaf ids are stable, and ordinals
  within a leaf are renumbered only for that leaf on edit. The build is
  an external merge sort, progressive, with runs in the cache dir. It is
  opt-in because it costs about 16 B/row.
- **Group view**: a group MA root is a small derived table. It paints
  as a grid of values, read-only.
- **Grid for a declared table**: sticky header shows schema *names*
  (hover or reveal shows the bound header text), exact row numbers
  (no `+N`), exact scrollbar, and computed columns as virtual trailing
  columns in the derived face that do not accept the caret. A **frozen
  left column** (the `key:` column) is chrome like the sticky header. The
  window fill still reads bytes (`rtx_layout_grid_walk`). The index only
  makes `grid_cam_lo` exact.
- **Tabs**: sheets (H1 sections) plus each external table. TUI shows
  one chrome line of tab labels, GUI a tab strip. A tab switch points
  the pane at the workbook doc (Rich, scrolled to that H1) or at the
  table doc (grid). Both are ordinary `RtxBuf`s in the workspace, and
  parking and journals work unchanged.
- **Formula bar**: chrome that shows the formula of the caret's cell,
  computed column or `calc` name, its type, its status (`pending 43%`,
  `⟳ table-wide`, the error provenance) and its dependents count. Edits
  in the bar are a `replace` on the workbook doc.
- **MD interop**: embedded tables are GFM tables, so outside a workbook
  they are ordinary MD tables, and inside one they gain names and
  formulas. md_table classify is window + lookback. Workbook tables are
  bound by the **whole-workbook parse**, so a table whose header is off
  screen still resolves. `Ctrl-L` on an embedded table opens it in grid
  (a view over the same bytes).

## 7. Concurrency

- **Build**: dest-live, like find and island (DESIGN §Scan). The frame
  never `.wait()`s. It pauses or resumes on focus the way `rtx_find_pause`
  does. Kick sets everything before starting (schema, parser, MA list).
  `RTX_PARALLEL_INLINE=1` runs it inline in CI.
- **Edit during build**: the tree is not safe to read under mutation.
  `replace` on a building table therefore does **cancel + wait** (bounded
  by one 2 MiB chunk per lane). It applies the edit through the hook to
  the leaves already built (below `built_off`) and shifts `built_off` by
  the delta when the edit is before it. It then re-kicks from
  `built_off`. An edit above `built_off` needs no index work, because
  the builder has not read those bytes yet. This is the `line_scan_off`
  `note_insert` pattern.
- **Recalc**: the DAG pass runs on the UI thread in the pump. It is
  O(dirty nodes) × merges and fits the frame. A full-MA rebuild (a new
  formula, a changed predicate or a non-linear scalar) is its own
  dest-live job over leaves: the sidecar if present, else a byte
  re-parse through the build loop. Its root publishes at `@stage`
  (monotonic `ma_off`), and dependents show pending until it finishes.
- **Publish discipline**: values, roots and `built_off` change only at
  `@stage` or on the UI thread. The frame reads no field a worker is
  still writing. This is the same rule as find's `scan_off`.

## 8. Phased plan

| Phase | Scope | Exit criteria / tests |
|---|---|---|
| **W0 Names** | `.wb.md` parse; embedded tables; `params`/`calc`; formula grammar and binder; DAG, Tarjan, topo recalc with cutoff; Rich paints values (md_view Derived value); error values; rename refactor | fixtures under `testdata/wb/`: every calc and cell value asserted; cycles reported with path; rename round-trips; recalc after one cell edit visits only dependents (counter smoke) |
| **W1 Index** | external `table` binding; header check; counted B+tree leaves; replace hook, split/merge, quote re-sync; progressive two-state parallel build; exact grid row numbers; `RTXI` persistence and validation | **1e8-row CSV (~6 GB)**: build ≥ 1 GB/s on 8 cores; reopen with cached index ≤ 50 ms; `g row N` ≤ 1 ms; RSS ≤ 64 MB above page cache; property smoke: 10k random edits, then index == fresh build (leaf-for-leaf positions) |
| **W2 Aggregates** (built for embedded tables: [W2 as built](#w2-as-built); zone maps and pending are W1's) | typed columns, zone maps, MAs (sum/count/avg/min/max), where filters, group MA, exact dec/float, pending propagation | single-cell edit in the 1e8 table updates `sum(where)` and dependents ≤ 2 ms; incremental == from-scratch (bit-identical) after random edits, undo, and region flips; `@perf_check` pins |
| **W3 Relations** | key/secondary indexes (RAM then LSM), lookups, computed columns, cross-table deltas, linear rewrite | Customers edit touches only leaves with that key (leaf-visit counter); `#dup` / `#schema` fixtures |
| **W4 Views & UI** | filter and sort views, frozen key column, tabs, formula bar (TUI and GUI), externalize apply | exact scroll over a 1e8-row filter view; tab switch keeps camera via `RTXC` |
| **W5 Scale** | columnar sidecar, append fast path, `use` imports, `export` verb | new MA over an indexed 1e8 table ≤ 2 s from sidecar; appending 1 GB to a log re-indexes only the tail |

Each phase is zero-cost when unused: a file outside a workbook never
touches the hook. The phases ship behind `@smoke` and `@perf_check`,
like the md_view wedges.

## 9. Risks, open questions, comparison

**Risks**

- *Quote re-sync cascades.* A stray `"` can re-flow everything after
  it. The mitigation is the convergence cap and "damaged from byte X".
  A paste of mixed quoting may stay damaged until the user fixes it.
- *Paste or delete of GBs* touches many leaves. The cost is linear in
  touched bytes, which is honest, and it runs as a job above
  `RTX_HL_WIN_MAX` with values pending.
- *Key index memory* at 1e9 unique keys requires the LSM. Until W3 lands
  it, lookups are limited to tables with ≤ 1e8 rows.
- *Float exactness* costs expansion arithmetic in leaves. `dec` is the
  recommended type for money.
- *External writers* (another process appending) are detected only
  through the identity triple at focus or save. Live tailing is out of
  scope.
- *DESIGN tension.* "Open does not scan the body" still holds, because
  the build is post-open and progressive. "No whole-file index" gains a
  named exception scoped to *declared tables*.

**Open questions**

1. ~~Rename over many sites~~ — decided: hist *groups* (one command,
   one undo step; see DESIGN [Edit groups](../DESIGN.md#edit-groups)).
2. ~~Should embedded-table type inference be locked into the text?~~ —
   W0: no; re-inferred per edit, a retype surfaces as a dependent's
   `#type` (see [W0 as built](#w0-as-built)).
3. Should leaf size be per-table (wide rows need bigger leaves) or
   fixed at `RTX_PAGE_SIZE`?
4. Is TSV/pipe parity enough, or are fixed-width and JSONL sources in
   scope for W5?
5. Persisted values (`RTXV`): does their cache make a cold workbook open
   show last-known values in a stale face before indexes validate?

**Comparison**

| Tool | Names / refs | Scale | Recalc | Truth |
|---|---|---|---|---|
| Excel Tables | `T[col]`, `T[@col]` (closest syntax) but A1 underneath; volatiles | 1,048,576 rows, in RAM | cell dependency chains | binary .xlsx |
| Numbers | header-name refs, named tables | small | cell | package |
| Airtable | typed fields, linked records (lookup) | ~100k rows/base | server | cloud |
| Causal | named variables and dimensions, model-first | model-sized | full | cloud |
| Quadratic | A1 + Python/SQL cells | browser memory | cell | cloud / file |
| Row Zero | A1 over billion-row sheets | large, server-side | server | cloud |
| DuckDB | SQL over CSV/Parquet, columnar | huge | re-run query (batch) | files |
| VisiData | column ops, freq tables | loads into RAM | re-run | files |

**What is new here.**

- **Truth is plain text you already own**: a diffable Markdown workbook
  and untouched multi-GB CSVs, edited in place.
- **Incremental view maintenance** (DBSP-style leaf deltas over a
  counted tree) runs *inside an editor*. A keystroke in a 10 GB file
  updates named aggregates in milliseconds, with no re-query.
- **Persistent, identity-validated indexes** mean a second open is
  instant.
- **Honest progressive values**: `≈ · 43%` is a first-class pending
  state and never a silently partial number.
- **Named, static, deterministic dependencies**: no A1, no volatiles,
  exact arithmetic, so every value is reproducible from bytes. All of
  it is local, with a few MB of RSS over the page cache.

## W0 as built

W0 (Names) landed: embedded tables, `params` / `calc`, the formula
language, the dependency graph, incremental recalc, the Rich lens and
rename. W1–W5 (external tables, the counted B+tree index, materialized
aggregates, views) are not built. Code: `core/wb.cch` / `core/wb.ccs`
(the engine, linked into the document target), the `RtxDerived` hook on
`RtxDoc` (`core/document.cch`), the layout's hint path
(`core/layout.ccs`), the status fragment and commands (`core/ui.cch`).

**Zero cost when unused.** `RtxDoc.derived` is set only by
`RtxDoc_from_path` / `from_base` for a `*.wb.md` path (or an explicit
`rtx_wb_attach`, as the tests do). Every hook site — the edit note in
`rtx_doc_span_note`, `has_marks`, `mark_edges`, the layout's
`hidden_to` / `hint_vis` / `reveal` and the edit patch — is one NULL
compare on any other file (`wb_smoke` asserts a `.md` and the same bytes
under another name get no hook).

**File format.**

- A sheet is an ATX H1 (`# Revenue`); text before the first H1 is an
  unnamed sheet. Setext headings are not sheets.
- `Table: Name` on its own line, then (blank lines allowed) a GFM table:
  a **named table**. Header cells are the column names (`` `list price` ``
  keeps spaces; backticks are dropped). A caption name may be backticked
  too (`` Table: `Price List` ``). A GFM table with no caption is plain
  Markdown and not part of the workbook. Columns stop at the lens's 16;
  a table over `RTX_WB_ROWS_MAX` (10 000) rows is refused: references to
  it are `#ref(… W1 indexes it)`.
- A body cell whose trimmed text starts with `=` is a formula; so is a
  code span `` `=…` `` (GitHub shows the formula as code). Anything else
  is a literal.
- ```` ```params ```` and ```` ```calc ```` fences hold `name = expr`
  lines; `#` starts a comment; other lines are inert. Params are global,
  calc names belong to their sheet. A ```` ```table ```` fence (an
  external table) is reserved for W1: it is structure, not a table yet.
- Nothing is ever written into the file; values live in the derived
  state and are rebuilt from bytes.

**Types.** A column's type is inferred from its literal cells (formula
cells do not vote): `int` ⊂ `dec(p)` (p = the most decimals seen) ⊂
`float` ⊂ `text`; `true` / `false` is `bool`, `YYYY-MM-DD` is `date`; a
column mixing numbers, dates and bools is `text`, and so is an empty one.
The counts per kind are kept per column, so an edit re-infers in O(1)
and re-types the column's literals only when the type moves.
Arithmetic: `int` and `dec` are exact (i128 inside, `#num` on
overflow); `*` adds scales (rounded half away from zero past 12); `/` is
`float` (`#div0` on zero); anything with a `float` is `float`. `date ±
int` days, `date - date` is days, `Nd` is N days. `&` concatenates as
text. A blank cell is `null`: arithmetic and comparisons with `null` give
`null`, aggregates skip it, `where` treats it as false,
`coalesce(a, b, …)` replaces it. Comparing different kinds is `#type`.

**Names.** Inside an aggregate's argument / `where`, or a lookup's
condition, a bare name is first a column of the table iterated; then a
calc name of the formula's sheet, a param, a table, and a sheet (only as
`Sheet.name`). `@col` is this row's cell (cells only); `T[@col]` is the
same written with the table. `T.col` is a column: a value only while
iterating `T`, else `#type(… aggregate it …)`. Names are case-sensitive;
two names in one scope that differ only in case (or repeat) are both
`#dup`.

**Formulas.** `sum` `count` `avg` `min` `max` `distinct` `any` `all`
over `T.col` or an expression of it, with an optional `where`
(`sum(Sales.amount where region = @region)`, `count(Sales where …)`,
`count(T)`); the table iterated is the first `T.col` / `T` in the
argument. `sum?(…)` (any aggregate + `?`) skips errors. `T[cond].col` is
a keyed lookup: 0 rows is `null`, 2+ rows `#dup` unless
`first(T[cond]).col`. `if c then a else b` (also `if(c, a, b)`),
`and` / `or` / `not`, `= != <> < <= > >=`, `+ - * / %`, and `abs`
`round(x[, n])` `floor` `ceil` `coalesce` `len` `lower` `upper`
`iserror` `iferror(x, y)` and `min` / `max` of two or more values.
`group` / `join` / `sort` / `top` parse to `#parse(… W2 / W4)` (W2 built
`group`; see [W2 as built](#w2-as-built)). No volatile functions: time is
a param (`asof`).

**Errors are values** with provenance: `#div0(division by zero in
Targets.gap[2])`, `#name(unknown name nope)`, `#ref(T has no column zz)`,
`#type(text + int in T.f[4])`, `#dup(Orders[...] matches rows 1 and 2
(first(...) takes one) in South.many)`, `#num`, `#parse(unexpected end
at column 5)`, `#cycle(Loop.a → Loop.b → Loop.c → Loop.a)`. An error
operand propagates unchanged, so a dependent shows where it came from.
The Rich lens paints the code (`#div0`); the status bar shows the
message.

**Graph and recalc.** Nodes are body cells, one node per column, and
calc / param names. A formula's edges come from binding (sorted, unique
node ids); a column node depends on each of its cells (implicit edges,
no storage). Tarjan SCC over those edges (iterative) gives the order —
dependencies first — and the cycles: every member of an SCC of two or
more nodes, or of a self-loop, is `#cycle(path)`, the path a shortest
walk from the SCC's root back to it. Recalc is a min-heap on that order
seeded with the changed nodes; a node whose new value equals the old one
(same kind, scale, bits) does not push its dependents. The order is
rebuilt (`regraph`, O(nodes + edges)) only when a formula's edges change.

**Edits are deltas.** The hook notes every replace (the same union as the
edit span). The next query syncs:

- **cell**: the union lies inside one body cell (between its pipes): that
  cell's bytes are re-read, rejected if a pipe, newline or trailing
  escape appeared; its literal is re-typed or its formula re-parsed and
  re-bound; regraph only if the edges changed; then the heap recalc.
- **line**: the union lies inside one calc / params line and the line
  still defines the same name: re-parse and re-bind that expression.
- **prose**: the union touches no block (a named table with its caption,
  a params / calc fence, another fence's opener or closer, an H1, a
  dangling caption) nor the line before or after one, and the new lines
  start no fence, H1 or caption: offsets shift, nothing recalcs.
- anything else — a new row, a header or separator edit, a new name, a
  fence — reparses the workbook (capped at `RTX_WB_MAX`, 8 MiB; over it
  values are off and the status says so). So does garbage from many
  incremental edits (model / value arenas past 16 MiB or 2× the build).

**Rich lens.** A formula span (a cell's trimmed `=…`, a calc
expression that is not a plain literal) is a hint whose stand-in is the
value: `hidden_to` / `hint_vis` read `RtxDoc_derived_at`, so table
widths, paint, `x_of` and hit follow in both frontends, and span edges
feed the hide cursor. The caret (or anchor) in a formula reveals that
formula alone — not the fence or code span around it — so the calc lines
beside it keep their values. A recalc's changed values relay their lines
in the edit patch and re-measure their tables; a reparse refills. In a
terminal a table cell that fits by what it paints stays one line.

**UI.** The status bar (both frontends) describes the workbook element
at the caret: `Targets.gap[2] = -54.40  (dec; 2 inputs, 1 dependent)`,
an error with its message, a column (`column Sales.amount (dec)`) or a
table. `F2` / **Workbook: Rename** (palette, Edit menu) opens the jump
prompt with the table / column / name at the caret — its definition or
any reference — and Enter renames it. **Workbook: Recalculate** reparses
and reports tables, formulas, nodes, cycles, errors and the time. No
formula bar yet (W4); no hover (the status is the message).

**Rename.** The definition (caption name, header cell, calc / param
name) plus every reference token the binder recorded (spans relative to
each formula), written as one `RtxDoc_replace_batch` group — one undo
step. A name that is not an identifier is backticked in formulas and the
caption. Refused, with nothing edited: an empty name, `` ` | # " `` or a
newline, the same name, or a name taken in its scope (another table or
sheet; another column of the table; another name of the sheet, or a
param). Capture across scopes (a param renamed to a column a `where`
reads) is caught after the edit: every node's bindings and values must
be unchanged, else the group is undone and the rename refused.

**Decisions on §9 and on what the design left open.**

- *Q2 (lock inferred types into the text):* no. Types are re-inferred on
  every edit (O(1)); a retype that breaks a dependent shows as that
  dependent's `#type`, never a silent coercion. A `types:` line lands
  with W1's schema syntax.
- *Q3–Q5:* not reached (W1 / W5). W0 persists nothing: values are
  recomputed at open (43 ms for 3000 rows / 9 000 formulas).
- *Division* is `float`; `dec` stays exact for `+ - *`.
- *Lookups* with no row are `null` (the design says "0 or 1 row").
- *Params are global, calc names per sheet*, resolved as above.
- *Cells hold formulas two ways* (`=x`, `` `=x` ``); both paint values.

**Tests.** `wb_smoke` (in `@smoke` / `@smoke_inline`): four fixtures in
`testdata/wb/` — `revenue` (the §2 example, embedded), `checks` (types,
errors, the scalar language), `cycles`, `scopes` (sheets, params,
qualified names, lookups, `first`, `#dup`, backticked names) — each
asserts every cell and name against its `.expect`; a counter test (one
cell edit reaches 19 nodes and visits exactly 15: cutoff stops at the
unchanged ones); the fast paths (cell, retype, formula, prose, calc line
with a cycle and its undo, a new row); the Rich lens (widths from values,
a recalc in another table patched, reveal); rename round trips (column,
table with a backticked name, a name from a reference, refusals, a
capture undone); and a 1500-step random-edit property test comparing the
incremental workbook with one built from scratch after every step.
`wb_perf` times the engine and keystrokes.

**Perf** (`wb_perf`, release, x86-64 shared host, p50): a workbook of
3000 rows × 7 columns with 9 000 row formulas and 10 calc aggregates
(185 KB, 21 018 nodes, 39 014 edges).

| Op | Time | Notes |
|---|---|---|
| open (parse, bind, Tarjan, full calc) | 43 ms | once per open |
| one literal edit → recalc | 0.66 ms | visits 11 nodes; the 10 aggregates rescan 3000 rows (no MAs in W0) |
| one formula edit (edges change) | 1.9 ms | re-bind + regraph |
| one prose keystroke | 0.003 ms | offset shift |
| a new row (structural) | 7.8 ms | reparse |
| literal edit, 1000 aggregates over the column | 47 ms | each aggregate rescans: W2's materialized aggregates remove this |
| keystroke in a table cell, type + Rich relayout | 1.93–2.10 ms | the same bytes as a plain `.md`: 1.25–1.28 ms |

**Limits (W0).** Aggregates rescan their table (no leaf partials); a
where-filter per row of another table is O(rows²) on edit. Structural
edits reparse the whole workbook. (W2 lifts the first and the row
inserts / deletes of the last: [W2 as built](#w2-as-built).) Values are not colored by kind or
error in the lens. Sheet rename, `types:`, `uses` imports, formula bar
and hover are later phases.

## W2 as built

W2 (maintained aggregates) landed for **embedded** tables, ahead of W1
(owner decision: W2 first, with the interface W1's on-disk leaves plug
into — [below](#the-ma-interface-for-w1)). Every aggregate in a formula
and every `group` is a **maintained aggregate (MA)**: a graph node whose
state is exact partials over its table's rows, kept current by row
deltas. A cell edit never re-reads the tables its aggregates cover.
Code: `core/wb.ccs` (the MA engine, the row path, groups),
`core/wb_num.cch` / `core/wb_num.ccs` (exact arithmetic), the annotation
in `core/layout.ccs` and both frontends' stand-in painters.

**What an MA is.** One per `sum` / `count` / `avg` / `min` / `max` /
`distinct` / `any` / `all` (and `?` forms) in a cell or calc formula,
nested ones included (`sum(A.v where v > avg(A.v))` is two MAs: the inner
one is a scalar input of the outer). Its inputs are of two kinds:

- **row columns** — columns of its table read in the row context (`v`,
  `A.v`, `region`): a changed cell queues its row on every MA that reads
  its column (`WTab.cma`), and at the MA's pop the row's **old**
  contribution is subtracted and the new one added. The old one is
  evaluated against the values from before this recalc (each changed
  node keeps `ov` until the recalc ends), so no per-row state is stored.
- **scalar inputs** — names, params, this row's cells (`@region`),
  lookups, fields, other aggregates: an ordinary graph edge. A change
  **rebuilds that MA with one walk of its table** (`where d >= asof` when
  `asof` moves). Zone maps are not built (optional, not cheap enough to
  pay for embedded tables).

The formula depends on the MA node, not on the columns, so W0's early
cutoff now also stops at an MA whose value did not change.

**A row's contribution** is one of: *skipped* (`where` false or null, a
null value, an error under `?`), a *stop* (an error value, or a type error
the aggregate cannot skip: `sum` over text, `where` not boolean, `any`
over a number — the aggregate's value is the **first stop row's error in
row order**, found through per-leaf stop counts, no walk), or a *value*.

**Partials.** Invertible, at the root: counts; the exact sums below;
`distinct`'s per-value counts (a key → count map; `distinct` is the
number of keys with a count); `any` / `all` true / false counts; min /
max's per-kind counts (a column mixing kinds is `#type`). `min` / `max`
keep a best row per **leaf** (a run of 64 consecutive rows; 128 splits,
under 16 merges into a neighbour — the W1 leaf shape) and a tree over the
leaves; a changed row only compares against its leaf's best, and losing
the extreme rescans that one leaf (≤ 128 rows), then O(log leaves) up the
tree. `count(T)` has no inputs at all: only row inserts / deletes move it.

**Exact numbers** (the owner's hard requirement: an MA maintained
through any edits, undo, row inserts and deletes is bit-identical to one
built from scratch):

- `int` / `dec(p)`: per scale, an exact 128-bit sum (a row adds one i64;
  2^32 rows cannot overflow it). The value combines them in a 256-bit
  integer of units 10^-18 (`RtxXd`) and reads it back at the widest scale
  present — exact, independent of order. Only a result that does not fit
  an int64 is `#num(sum too large)` (W0's i128 bound on partial sums
  depended on order; W2's does not).
- `float`: a **superaccumulator** (`RtxXs`): a fixed-point integer in units
  of 2^-1074 spanning the whole double range, as 70 signed 64-bit limbs of
  32 bits each (Radford Neal's "small accumulator": each add touches three
  limbs, carries wait, a renormalise every 2^30 adds). Adding or
  subtracting a finite double is exact; the value is the exact real sum
  **rounded once, to nearest, ties to even**. Subtraction is the same
  add with the sign flipped, so a delta is exact.
- `int` / `dec` mixed with `float` in one aggregate: the exact real sum of
  every value — the decs as exact decimals, not as rounded doubles — then
  one rounding (`rtx_xs_round_mixed`: the float part × 10^18 plus the dec
  part × 2^1074, divided once).
- **Non-finite rules** (counted apart from the accumulator, so they are
  invertible too): any NaN, or +inf and -inf together, makes the sum the
  canonical quiet NaN (`0x7ff8000000000000`, one bit pattern); else any
  +inf is +inf, any -inf is -inf. A finite sum past DBL_MAX rounds to
  ±inf (IEEE overflow: the halfway point to 2^1024 goes to inf, ties to
  even). An exact zero is +0.0 (so -0.0 alone sums to +0.0).
- `avg` is `sum / count` over that exact sum (a double division); `count`
  is exact.
- `min` / `max`: numbers compare **exactly** across `int` / `dec` /
  `float` (a dec against a float is decided by the exact difference, not
  by converting); ties keep the **earliest row**; any NaN among the values
  makes the result the canonical NaN (W0 kept whichever came first — an
  order dependence).
- `distinct` keys numbers by exact value (`2`, `2.00` and `2.0` are one
  key; a float that is not a decimal of scale ≤ 18 keys by its bits),
  text by bytes, one NaN key.

**Rule changes from W0** (each W0 one depended on row order or history):
row errors take precedence over a mixed-kinds `min` / `max`; NaN in `min`
/ `max`; the `#num` bound on sums; a dec mixed into a float sum adds as an
exact decimal. Every W0 fixture is unchanged except `checks`' `group`
line, which now has a value.

**Groups.** `name = group T by col { out: agg(...), ... }` (a calc or
param name only; the root of its formula) is one group MA: a map from
the key (the column's value, keyed as `distinct` keys) to one partial per
output. `count()` counts the group's rows; any aggregate over bare column
names of `T` works, `where` and `?` too. Read a group with a keyed
lookup, `by[region = "EU"].total` (a missing key is `null`); the dump
lists every group in key order, and the Rich lens paints `3 groups`. Its
value carries a version that moves when any group changes, so its
lookups re-read (O(1) each) and cut off at their own values. A group's
`min` / `max` that loses its extreme recomputes that key with one walk;
a key error stops the whole group (its first stop row's error). Editing a
group's definition (not its table) re-binds like any formula; `join` /
`sort` / `top` still parse to `#parse(… W4)`.

**Rows as deltas.** An edit that replaces whole body rows of one table
with pipe rows — a new row, a deleted one, a moved one (a swap in one
replace), several at once, a row whose cell count changed — takes the
**row path**: every MA over the table subtracts the old rows'
contributions (current values), their cell nodes die, the new rows get
nodes (ids appended; a row's position maps to its cells through
`WTab.rowcell`), their formulas bind, and each MA takes them as new rows
at its next pop. The leaves take the rows (split / merge; every MA's leaf
arrays mirror them). Error cells below the edit re-evaluate (their
messages name row numbers); so do cycle paths. The header, separator,
caption, a fence, a row that would end or split the table, a table at EOF
without a newline, and 0 or over 10 000 rows still reparse. Dumps and
cycle paths follow document order, not node ids, so a workbook edited
this way and one built from scratch print the same.

**Pending** stays a W1 bit: `WMa.pending` exists and is never set —
embedded tables have no asynchronous build. W1's progressive build sets
it while an MA covers a prefix, and dependents carry it (§5).

**`first` and lookups** are not maintained: `T[cond]` and `first(...)`
depend on the columns they read and rescan their table when one changes,
as in W0 (a key index is W3).

### The MA interface for W1

The engine is written against leaves, so W1's counted B+tree plugs in
where `WTab.lvn` / `lvlo` / `rleaf` (rows per leaf, first row, row →
leaf) are today:

| Piece | Embedded (W2) | W1 plug-in |
|---|---|---|
| a row's contribution | `wb_con(ctx, WOut, row)`: evaluates `where` / the argument over the row's cells | the same over a record parsed from the leaf's bytes; the old record is parsed from the bytes captured pre-replace |
| the partial | `WAcc` + `acc_apply(±1)` + `acc_value` | unchanged: a leaf keeps a `WAcc` (with its `RtxXs` / `RtxXd`), an inner node merges children with `rtx_xs_merge` / `rtx_xd_merge` and count adds — exact, so the root is the same whatever the tree shape |
| stop rows | per-leaf `lstop`; the first stop row by walking leaves in order | the same counts summed up the tree; descend to the first leaf with one |
| min / max | per-leaf `lbest` (value + row) under a segment tree (`seg`) | the leaf best in the leaf partial, merged up the B+tree (left wins ties) |
| structure | `lv_ins` / `lv_del` / `lv_split` / `lv_merge` mirror every MA's arrays | the index hook's leaf split / merge |
| deltas | `ma_dirty(rid)` queues a row; `ma_apply` does old-out / new-in | the leaf hook's `(leaf, old partial, new partial)`: subtract and add the leaf's partial up the path |

### Live value annotation

While the caret is in a formula — a cell's `=…` or a calc expression —
its value paints **after** it as a dimmed annotation: `s = sum(T.a) → 4`,
in Rich (where the formula is revealed) and in Source, in both
frontends. Each keystroke re-binds and re-evaluates that formula through
the incremental path and the annotation shows the new result at once; a
half-typed formula shows its error with the message (`→ #parse(expected ,
or ) at column 8)`); dependents update as before. When the caret leaves,
the value paints in place again (Rich) or nothing shows (Source).

- It holds **no bytes**: it rides as the stand-in of the formula's last
  cluster (`RtxHintVis.ann`: that cluster's glyphs, then ` → value`), so
  copy, search, undo and save never see it (the PTY test checks the file).
- The caret at the formula's end sits **before** it (`x_of` and the TUI
  cursor stop at the cluster's glyphs); a click on it lands at the
  formula's end; selection paints only real glyphs.
- It is dimmer than text: the theme entry `rtx_theme_annot()`
  (`comment.annotation` scope; `\e[38;5;244m` in a terminal).
- **Layout decision:** in a table the annotation **widens the column**
  while it shows (the table fit already re-fits per fill; when the pane
  is too narrow, the cell wraps inside the fitted column). Wrapping the
  annotation inside a fixed column instead would change the row's height
  on every value-length change and move every line below; widening moves
  only that table's columns, and only while the caret is in the cell.
- Cost: typing in a formula cell with the annotation showing is
  3.7 ms p50 against W0's 3.9 ms for the same keystrokes without it (the
  rebind + regraph of a formula edit dominates both); typing in a
  literal cell is 2.1 ms (W0 2.8 ms).

### Tests

- `wb_smoke`: the four W0 fixtures (only `checks`' `group` line changed);
  the W0 counter now asserts 18 visits (the 7 MAs, and 4 formulas whose
  MA kept its value are cut off) and that the edit walks no table;
  `xsum` — `testdata/wb/xsum.txt` (`tests/wb_xsum_gen.py`: Python's
  exact `Fraction` sums rounded once) — every float set summed forward,
  backward, shuffled, with junk added and subtracted, and as two merged
  halves, all bit-equal to the reference; mixed float + dec; exact dec
  sums (two routes); exact dec / float comparison; `counter_1000` — one
  cell edit under 1000 aggregates evaluates each MA's row exactly twice
  (old, new), builds none, rescans no leaf, and removing the max's
  extreme rescans one leaf per max MA; `groups_rows` — group values,
  listing, a two-row insert and a delete as deltas, undo; `annotation` —
  the stand-in, x_of / hit before it, keystrokes, a half-typed error,
  Source, a table cell (its column widens), no bytes.
- `wb_prop_smoke` (new, in `@smoke` / `@smoke_inline`): 4 seeds × 1500
  random steps over an MA-heavy workbook — cell edits with int / dec /
  float values including 1e308, 4.9e-324, -0e0, 1e999 (inf), NaN from
  `@x - @x`, huge decs, text, errors; keystrokes; param changes that flip
  predicates; calc formula changes; row inserts, deletes, swaps, 3-row
  deletes; header edits (reparse); undo / redo. After every step, every
  value and every MA of every formula (floats as raw bits) equals a
  workbook built from scratch.
- `tui_pty_test.py wb_annotation`: the annotation in a terminal —
  appears, dims, follows keystrokes (`+`, `1`, `0`: `#parse`, `5`, `14`),
  a half-typed error, Source, a table cell, the cursor before it, gone
  when the caret leaves, never in the file.

### Perf

`wb_perf`, release, same shared host, base (W0, `b8b7e3e`) and W2
interleaved, best p50 of 4 runs: 3000 rows × 7 columns, 9 000 row
formulas, 10 calc aggregates (the `aggs` workbook adds 1000 calc
aggregates over one column).

| Op | W0 | W2 | Notes |
|---|---|---|---|
| one cell edit, 1000 aggregates over its column | 101 ms | **0.70 ms** | each MA: one O(1) delta (2 row evaluations) |
| one literal edit | 0.94 ms | 0.22 ms | the 10 aggregates take deltas, no rescan |
| a new row / its delete | 14.8 ms | 1.95 ms | row path, no reparse (regraph for the row's formulas) |
| a param a `where` reads (`k`) | 0.34 ms | 0.45 ms | that MA's one walk (W0 rescanned too; the MA also rebuilds its leaves) |
| one formula edit (edges change) | 1.9 ms | 1.6 ms | |
| one prose keystroke | 0.004 ms | 0.004 ms | |
| open, 3000 rows | 66 ms | 68 ms | MAs built in the initial pass |
| open, + 1000 aggregates | 154 ms | 180 ms | 1000 walks either way; MA bookkeeping |
| keystroke in a table cell, type + Rich relayout | 2.79 ms | 2.06 ms | plain `.md`: 1.75 ms |
| keystroke at a formula's end (annotation on in W2) | 3.92 ms | 3.72 ms | plain `.md`: 1.76 ms |

**Decisions.**

- W2 before W1, for embedded tables, over leaves shaped like W1's.
- One MA per aggregate occurrence (not shared across formulas): errors
  raised inside keep naming their formula (`in Targets.actual[2]`), as in
  W0. The §4 rewrite of `where region = @region` across rows into one
  group MA is not automatic; `group` is explicit.
- Old contributions come from the values before the recalc, not from
  stored per-row state: no memory per row × MA.
- A scalar input's change rebuilds the MA with one walk; zone maps are
  not built.
- Group lookups are `G[key = value].out` (the design's keyed-lookup form).
- The annotation widens a table column rather than wrapping in it.

**Limits (W2).** `first` / lookups rescan (W3 key index). Zone maps are
not built. Editing a group's definition reparses the workbook (its
readers bind its outputs by name). Inserts into a table with no body
rows reparse. A scalar input's change costs its MA one walk of the table
(≤ 10 000 rows for an embedded table).

## Locks

| Topic | Lock |
|---|---|
| Truth | Workbook text and source bytes. Values, indexes and views are derived and rebuildable; values never go into the file |
| References | Named tables, columns, rows and scalars; no A1; static edges; no volatile functions; `asof` is a param |
| Recalc | DAG over names; topological order; early cutoff; cycles are `#cycle(path)`; no iteration |
| Change unit | Leaf delta on a counted B+tree. Invertible aggregates add/subtract; min/max re-merge ancestors |
| Index exception | A table **declared in a workbook** gets a whole-table index. Plain grid stays windowed (`core/grid.cch`) |
| Build | Dest-live Scan-table row; two-state parallel parse; publish at `@stage`; partial is `pending`, never final |
| Persistence | `RTXI` in the cache dir; identity triple + schema/parser hash + sampled leaf hashes; unknown ver ignored |
| Edits | `replace` hook on the table doc; cancel+wait a live build, patch built leaves, re-kick |
| Arithmetic | `dec` exact (per-scale i128, a 256-bit total); `float` exact superaccumulator, rounded once (NaN / ±inf by count); results independent of edit history and row order |
| Non-goals | A1 grids, macros, volatile functions, collaborative OT, server engines, writing values into CSVs |

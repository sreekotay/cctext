# Workbooks: named tables, dependency recalc, giant data

**Status: W0 (Names), W2 (Aggregates) and W1 phase 1 (the lazy engine for embedded tables, to 10M rows) landed** — see [W0 as built](#w0-as-built): embedded named tables, `params` / `calc`, the formula language, the DAG with Tarjan and cutoff recalc on edit deltas, values in the Rich lens, rename; and [W2 as built](#w2-as-built): maintained aggregates updated by row deltas with exact float / dec sums, `group`, rows inserted and deleted as deltas, and the live value annotation while the caret is in a formula; [W2 at scale](#w2-at-scale): embedded tables to 1M rows, a structural reparse off the keystroke path, and what W1 must provide for 10M. [W1: the lazy engine](#w1-the-lazy-engine-design) redesigns the engine after the highlighter (compute for the view, checkpoints, convergence, background jobs, anchors for fixed rows); [W1 as built](#w1-as-built): phase 1 for embedded tables landed — ~21 B of index a row (10M rows open and edit), cooperative cancellable jobs with a stale display, running totals with checkpoints, anchors, the numeric contract. W3–W5 are design. Open questions in §9 (decisions for W0 there).

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
| Parsed workbook: names, schemas, formulas, graph | derived | workbook epoch (arena on the workbook `RtxDoc`) | full parse of the workbook doc (capped `RTX_WB_MAX`: 8 MiB in W0, 256 MiB since [W2 at scale](#w2-at-scale)) |
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
| **W1 Lazy engine** (redesigned: [W1: the lazy engine](#w1-the-lazy-engine-design); phase 1 is embedded tables) | the rule graph with shapes, no per-cell state, block partials, checkpoints, view-first evaluation and background jobs; phase 2: external `table` binding; header check; counted B+tree leaves; replace hook, split/merge, quote re-sync; progressive two-state parallel build; exact grid row numbers; `RTXI` persistence and validation | **1e8-row CSV (~6 GB)**: build ≥ 1 GB/s on 8 cores; reopen with cached index ≤ 50 ms; `g row N` ≤ 1 ms; RSS ≤ 64 MB above page cache; property smoke: 10k random edits, then index == fresh build (leaf-for-leaf positions) |
| **W2 Aggregates** (built for embedded tables to 1M rows: [W2 as built](#w2-as-built), [W2 at scale](#w2-at-scale); zone maps and pending are W1's) | typed columns, zone maps, MAs (sum/count/avg/min/max), where filters, group MA, exact dec/float, pending propagation | single-cell edit in the 1e8 table updates `sum(where)` and dependents ≤ 2 ms; incremental == from-scratch (bit-identical) after random edits, undo, and region flips; `@perf_check` pins |
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
  a table over `RTX_WB_ROWS_MAX` rows (10 000 in W0; 1 000 000 since
  [W2 at scale](#w2-at-scale)) is refused: references to it are
  `#ref(… W1 indexes it)`.
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
  fence — reparses the workbook (capped at `RTX_WB_MAX`, 8 MiB in W0 and
  256 MiB since W2 at scale; over it values are off and the status says
  so; from 1 MiB the reparse waits for a pause in typing, see
  [W2 at scale](#w2-at-scale)). So does garbage from many
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
without a newline, and 0 or over `RTX_WB_ROWS_MAX` rows still reparse. Dumps and
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
  literal cell is 2.1 ms (W0 2.8 ms). Since the scale work (a formula
  edit placed locally in the order, no Tarjan) it is 2.0 ms, against
  1.8 ms for the same keystrokes in a plain `.md`.

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
aggregates over one column). The "W2 at scale" column is one later run
of the same tool (p50) after the scale work below.

| Op | W0 | W2 | W2 at scale | Notes |
|---|---|---|---|---|
| one cell edit, 1000 aggregates over its column | 101 ms | **0.70 ms** | 0.54 ms | each MA: one O(1) delta (2 row evaluations) |
| one literal edit | 0.94 ms | 0.22 ms | 0.10 ms | the 10 aggregates take deltas, no rescan |
| a new row / its delete | 14.8 ms | 1.95 ms | 0.12 ms | row path, no reparse; since W2 at scale no regraph for own-row formulas |
| a param a `where` reads (`k`) | 0.34 ms | 0.45 ms | 0.19 ms | that MA's one walk (W0 rescanned too; the MA also rebuilds its leaves) |
| one formula edit (edges change) | 1.9 ms | 1.6 ms | 0.11 ms | placed in the order locally, no Tarjan |
| one prose keystroke | 0.004 ms | 0.004 ms | 0.004 ms | |
| open, 3000 rows | 66 ms | 68 ms | 47 ms | MAs built in the initial pass |
| open, + 1000 aggregates | 154 ms | 180 ms | 184 ms | 1000 walks either way; MA bookkeeping |
| keystroke in a table cell, type + Rich relayout | 2.79 ms | 2.06 ms | 1.95 ms | plain `.md`: 1.75 ms |
| keystroke at a formula's end (annotation on in W2) | 3.92 ms | 3.72 ms | 1.96 ms | plain `.md`: 1.76 ms |

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
(up to `RTX_WB_ROWS_MAX` rows for an embedded table: ~20 ms at 100k).

## W2 at scale

Giant embedded workbooks: how far W2 goes without W1's on-disk index,
what it costs there, and what stops it.

**The generator** (`tests/wb_gen.cch`): one table `Big` of N rows —
`id` int, `key` text (5 keys), `a` int, `b` dec(2), `c` float with
awkward values (1e-300, 1e300, -2.5e10, 7e-5), `d` date, `e` int, and two
row formulas (`f1 = @a * @b`, `f2 = @f1 + @c`) — params `k` / `asof` /
`lim` that predicates read, and a calc fence of 1000 aggregates over `a`
(sums with `where id > n`, averages by key, counts, maxes), 100 over `b`,
one over `e`, a `group` with two lookups, and a chain of 50 names. It
writes to memory or streams to a file (`wbgen_file`: a 10M-row file is
never held).

**What changed for scale.**

- *Row ids.* A cell names its row by a stable id (`WTab.rpos` maps it
  to a position), so a row insert rewrites one position per row after
  it, not every cell's; error cells below the edit re-evaluate only when
  the table has any (`WTab.nferr`). 100k rows: 43 → 0.6 ms (no
  aggregates).
- *Memory.* The node array lives on the heap (an arena doubling left
  every old copy behind until the next rebuild), is reserved per table,
  and a node is 160 bytes (32-bit offsets: the byte cap stays under
  4 GiB; cycle values and a recalc's old values in side tables). Row
  formulas that read only their own row, literals and operators
  (`=@a * @b`) share one bound AST per column; their deps come from the
  AST at bind. 100k rows: 727 → 279 MB (no aggregates).
- *Open.* MAs over one table build in one pass over its leaves (each
  leaf's cells read once from memory for all of them), not one walk
  each: 100k rows × 1110 MAs, 27.8 → 6.0 s.
- *Caps.* `RTX_WB_ROWS_MAX` 10 000 → **1 000 000** rows,
  `RTX_WB_MAX` 8 MiB → **256 MiB**. `rtx_wb_limits` moves them at run
  time (tests, the perf tool).
- *The reparse leaves the keystroke path.* A structural edit (header,
  separator, fence, caption, a table's end, garbage) reparses the whole
  workbook: ~0.6 s at 10k rows with 1000 aggregates, 5.6 s at 100k,
  64 s at 1M — far over ~100 ms. From `RTX_WB_DEFER_BYTES` (1 MiB,
  ~12k rows of the generated table) the derived hooks and the status
  line do not reparse: the values hide (the layout sees no derived marks,
  so nothing stale paints), the status says `workbook: recalculating
  when typing pauses`, and the host's idle loop (`rtx_ui_wb_idle`, both
  frontends, after `RTX_WB_DEFER_MS` = 300 ms without an edit) reparses
  and relays the views. An explicit query (`rtx_wb_eval`, stats, dump,
  rename) reparses at once. The reparse still runs on the UI thread when
  it runs; making it concurrent needs the model built off a byte
  snapshot and swapped in, with the edits made meanwhile replayed —
  W1's progressive build is that machinery.

**Tests.**

- `wb_scale_smoke` (in `@smoke`, `@smoke_asan`, `@smoke_tsan`; 20k rows
  under a sanitizer): 100k rows, 30 aggregates, a 10 000-step storm —
  cell edits in the int / dec / float columns, row inserts and deletes,
  undo / redo, param flips, calc-line and row-formula edits — with each
  step's counters asserted (the cell path, no MA walks its table, at most
  8 visits for an edit under one aggregate and 200 otherwise, row edits
  on the row path with no regraph, a param flip walking at most its one
  MA, no reparse in the whole storm) and every value and MA compared
  bit for bit with a scratch build every 3334 steps. About 14 s.
- `wb_smoke`: the byte cap (lowered to 8 MiB for the test), the row cap
  (a row insert over it refuses the table with the cap in the message;
  undo restores it), and the deferred reparse (hidden, `rtx_wb_due_ms`,
  idle before and after due, a query reparsing at once).
- `tui_pty_test.py wb_deferred`: a 70k-row workbook in the terminal — a
  structural edit hides the values and says so; the idle loop repaints
  them.

**Perf** (`wb_scale_perf`, release, a shared 4-core host; p50 of 20
samples, 100 for keystrokes; 1000 + 100 + 10 aggregates):

| Op | 10k rows | 100k rows | 1M rows |
|---|---|---|---|
| file | 0.8 MB | 8.1 MB | 82 MB |
| open (parse, bind, graph, 1110 MA builds) | 0.59 s | 6.1 s | 60–69 s |
| RSS after open | 36 MB | 338 MB | 3.55 GB |
| cell edit, 1 aggregate over its column | 0.014 ms | 0.015 ms | 0.022 ms |
| cell edit, 100 aggregates | 0.041 ms | 0.043 ms | 0.051 ms |
| cell edit, 1000 aggregates | 0.41 ms | 0.43 ms | 0.43 ms |
| row insert / delete | 1.0 ms | 9.2 ms | 84 ms |
| param flip (one MA walks) | 0.61 ms | 22 ms | 165 ms |
| full reparse (idle loop, not the keystroke) | 0.61 s | 5.6 s | 64 s |
| keystroke in a Rich table cell (TUI measure) | 4.8 ms | 13.6 ms | 13.9 ms |
| same, GUI-like pixel measure | 4.7 ms | 13.6 ms | 13.3 ms |
| keystroke at a formula's end (annotation) | 2.5 ms | 10.8 ms | 12.1 ms |
| PageDown in Rich | 3.6 ms | 11.2 ms | 11.4 ms |

Cell edits are flat in the row count: an edit touches its MAs' deltas
and nothing that scales with the table. The row path at 1000 aggregates
is dominated by every MA's leaf arrays following the leaves
(O(MAs × leaves): 0.6 ms at 100k rows with no aggregates). A param flip
is its one MA's walk (~0.16 µs a row). A keystroke is the editor's
Markdown pass and Rich table fit (9.1 ms a keystroke and 7.5 ms a
PageDown over a 10M-row table with the workbook off) plus, with it on,
a refill of the view when an edit changes more than 64 painted values
(here every aggregate reading the column): 11–14 ms from 100k to 1M
rows. The annotation adds nothing measurable.

**Pins.** `@perf_check` runs `scripts/wb_perf_pins.sh check` first:
`wb_scale_perf --pins 100000` against `testdata/perf/wb_pins.env`, one
pin per op (p50 × 2, at least 0.1 ms) and the open RSS (× 1.25); a p50
over pin × `RTX_PERF_FACTOR` (3) fails. `@wb_perf_check` runs it alone;
`@perf_record` refreshes it.

**Tiers: 5M and 10M.** `wb_scale_perf 2000000` (open only): a 165 MB file opens in 157 s to 7.1 GB RSS (peak 7.6 GB) — the largest tier under the 8 GB guard on this 15 GB host. `5000000` and `10000000` (414 MB and 829 MB, streamed to `/tmp` in 2.5 s and 4.9 s after checking 11.3 GB free, deleted after the run): the predicted model (17.4 GB, 34.4 GB) is over the guard, so the tool does not build it. Opened as shipped, the file is over the byte cap and the values are off (2 ms); with the byte cap lifted, the table is refused and the rest of the workbook parses in 1.3 s / 2.6 s at 17 MB RSS (`total` reads `#ref(Big has more than 1000000 rows (W1 indexes it))`); the Rich lens over the 10M-row table alone (workbook off) takes 9.1 ms a keystroke and 7.5 ms a PageDown. Nothing ran out of memory: the tool predicts before it builds, and a watchdog thread stops the run cleanly (file removed) if RSS passes the guard anyway.

**What stops embedded tables, exactly.**

1. *Memory per row.* ~3.5 KB a row at 1110 aggregates: 9 cell nodes of
   160 B (1.44 KB), their text and the row arrays (~0.2 KB), and every
   MA's per-leaf state (leaf counts, stop counts, min / max per leaf and
   its segment tree: ~1.5 KB a row at 1110 MAs over 64-row leaves). 1M
   rows is 3.55 GB; 5M would be ~18 GB and 10M ~35 GB — past this host's
   15 GB and the 8 GB guard. With no aggregates it is ~2.8 KB a row.
2. *Open / reparse time.* Parse and bind are ~10 s per million rows;
   each aggregate adds a walk (batched per leaf: ~50 ns a row per MA), so
   1000 aggregates cost a minute per million rows at open and at every
   structural edit.
3. *The caps.* 1M rows / 256 MiB as shipped (a 10M-row file is ~0.8 GB:
   over the byte cap its values are off; with the byte cap lifted the
   table is refused and references read `#ref(Big has more than 1000000
   rows (W1 indexes it))`). Node offsets are 32-bit, so 4 GiB is the
   hard ceiling for one embedded workbook.
4. *The layout* is not the blocker: a keystroke in a Rich table cell is
   13.9 ms at 1M rows and 9.1 ms over a 10M-row table (workbook off),
   PageDown 7.5 ms.

**What W1 must provide for 10M rows.**

- *No node per cell.* A table is its bytes plus a counted B+tree of
  leaves (row ranges with byte offsets); a record is parsed from the
  leaf's bytes when evaluated. Row formulas are one AST per column
  (as W2 already shares) with per-row values computed per leaf, not
  stored per cell. Target: ~100 bytes a row of index (10M rows ≈ 1 GB,
  most of it evictable leaf caches), not 3.5 KB.
- *Aggregate state per leaf block, not per MA per 64 rows.* Per-leaf
  column summaries (count, exact sum partials, min / max, zone maps)
  shared by every MA over the column; per-MA partials only for MAs with
  predicates, and at a coarser grain (4096-row blocks: 10M rows × 1110
  MAs ≈ 170 MB). Zone maps let `where id > n` skip whole blocks.
- *A progressive, concurrent build* over a byte snapshot, publishing at
  stages with `pending` set (the `WMa.pending` bit and the MA interface
  above are the hook points), cancel-and-rekick on edits, so open and
  structural edits never block the UI thread; the persisted `RTXI` makes
  a reopen O(leaves).
- *Offsets past 4 GiB*: 64-bit leaf offsets (cells relative to their
  leaf, as W2 already stores them relative to their row).

## W1: the lazy engine (design)

W2 at scale showed what stops embedded tables: a graph node and a stored
value per cell (~3.5 KB a row at 1110 aggregates), a reparse that
rebuilds every node, and one walk per aggregate that must finish before
anything paints. W1 replaces that engine. Its computation follows the
pattern cctext already uses for lazy syntax highlighting (DESIGN
[Interactive](../DESIGN.md#interactive), `rtx_doc_hl_extend`,
`RtxWs_hl_trim` / `RtxDoc_hl_trim`, the checkpoint relex and the
dest-live scans of the [Scan](../DESIGN.md#scan) table): **compute what
the view shows, keep checkpoints instead of results, stop at
convergence, never block a keystroke**.

This section is the design. Phase 1 (embedded tables) implements it;
[W1 as built](#w1-as-built) records what landed and its numbers. External
multi-GB tables and cross-workbook imports are phase 2.

### The lazy pattern

| Highlighting (cctext today) | Workbook (W1) |
|---|---|
| Lex the window (visible rows + lookback), not the file | Evaluate the visible cells, the status line's cell and what they read — nothing off-screen unless an aggregate or something visible needs it |
| Checkpoints every 4 KiB / 1 KiB: the lexer state at a byte | Block partials every 256 rows: an aggregate's partial (counts, exact sums, min / max, stop rows) and a relative-row chain's running state at the block's end |
| An edit relexes from the last checkpoint before it and stops at the first checkpoint past it whose state equals the old one | An edit re-evaluates from the edited row's block; a chain stops at the first block whose end state equals its old checkpoint; a value that recomputes equal stops its dependents (W0's cutoff, per block) |
| Windows beyond the lookback are lexed on demand (`hl_extend`, the run working set) | Rows beyond the checkpoint frontier are evaluated on demand when the view reaches them, within a frame budget; farther rows show their last value, dimmed, until the job reaches them |
| `RtxDoc_hl_trim` drops runs far from every camera; checkpoints stay | The view cache drops cell values far from every view (a bounded cache); partials and checkpoints stay |
| Dest-live scans (find, island, project walk): stamped, cancellable, publish at `@stage`, resume from a cursor | Background jobs (a big aggregate rebuild, a chain recompute, the structural read): stamped with the edit generation, stepped in slices off the keystroke path, cancellable (Esc, **Workbook: Cancel Recalculation**), resuming from a block cursor |
| Bytes are truth; runs / sections / checkpoints are derived | Bytes are truth; the position index, partials, checkpoints and key indexes are derived; no value is stored per cell |

### What is stored

Only bytes plus four derived structures (none of them per cell):

1. **The position index** — per table, blocks of about 256 rows (split
   above 512, merged below 64). A block stores its first byte and, per
   row, the row's offset in the block, its length and a stable row id;
   a row id maps back to (block, slot). Offsets are relative, so an edit
   moves O(blocks) numbers, not every row after it (the W2 leaf shape,
   now the only per-row state).
2. **Block partials** — per aggregate instance and block: the stop-row
   count, the best row for `min` / `max`, and for aggregates over a
   relative-row chain the whole partial. The aggregate's root partial
   (the W2 `WAcc`, exact sums included) is the sum of its blocks.
3. **Checkpoints** — per table with a previous-row chain: the carried
   values (the last K rows of every chain column) at each block's end,
   plus the frontier below which they are known good.
4. **Key indexes** — per (table, column) that an equality lookup reads:
   key → row ids. Both sides of a keyed lookup are indexed.

Plus per workbook: the parsed names, the rules and their bound ASTs, the
rule graph, one value per name / aggregate instance (the "last value"),
and a bounded view cache of visible cells.

A row formula's value is recomputed from the row's bytes whenever it is
needed (a paint, an aggregate's row contribution, a lookup). The old
value a delta subtracts is recomputed from the edited row's **old
bytes**, captured by a pre-edit hook before the document changes:
formulas are pure, so the row's old inputs determine its old value.

### The rule graph

Dependencies are tracked between **rules**, not cells. One node per:

| Node | Example | Holds |
|---|---|---|
| name | `eu_total = …` in a `calc` / `params` fence | a value |
| rule | a column's formula text: every row whose `f` cell is `=@a * @b` shares one rule | a bound AST and a row count; no value |
| column | `Big.f` — the union of its literals and its rules | nothing (a hub) |
| aggregate | `sum(Big.a where id > k)`; `group …` | one instance per distinct outer-row parameter tuple (one when it reads no `@col`); each instance holds partials |
| anchored cell | `Big#q3.total`, `Big#last[-1].total` | a value |

Edges carry a **shape** that maps rows of the dependency to rows of the
reader:

| Shape | Written | Storage | On a change of the dependency |
|---|---|---|---|
| same row | `@a * @b` | none (implicit) | only that row of the reader; within a row, cycles are found while evaluating |
| whole column → aggregate | `sum(T.a)` | block partials + root | row delta: old contribution out, new in, O(1) per aggregate; `min` / `max` losing the extreme rescans one block, then O(blocks) |
| scalar → rule | `@a * k`, `where d >= asof` | none | the whole dependent is stale: every row of a rule (its column's aggregates rebuild), every block of an aggregate — a background job when big |
| lookup by key | `Customers[id = @cust].tier` | key index on both sides | only reader rows whose key equals the edited row's old or new key |
| relative rows | `@total[-1]`, `@x[+2]` | chain checkpoints per block | the reader rows at the offsets; a chain is dirty from the edited row on, recomputed lazily, stopping at convergence |
| anchored row | `T#q3.col`, `T#q3[+1].col`, `T#first.col` | the anchor index (name → row id) | the anchored cell node re-reads its one cell; it moves with its row |
| anchor range | `sum(T[#q1 .. #q3].amount)` | block partials + two edge blocks | a row delta inside the range; an anchor moving re-reads the two edge blocks |
| group | `group T by k { … }` | per-key partials | the edited row's old key and new key |

A formula whose shape cannot be determined statically is rejected when it
is bound, with a message naming the reference (`@col[k]` needs a whole
number of rows, at most 64; a chain that reads later rows (`@x[+k]`) back
into itself is refused). There are no dynamic or string references.

**Cycles.** Tarjan runs over the rule graph (a few thousand nodes at
most, so it runs whenever a rule appears or goes). A strongly connected
component whose edges are only same-row and previous-row edges of one
table is not a cycle: previous-row edges make a **chain** (well founded:
it only reads earlier rows), and a same-row loop is found per row while
evaluating (`T.a[3] → T.b[3] → T.a[3]`, exactly as W0 reported it). Any
other component — through an aggregate, a name, a lookup or an absolute
cell — is a cycle: every member is `#cycle(path)`, the path starting at
the member first in document order and naming a rule by its first row
(`C.z[1] → C.z → C.z[1]`, W0's text). Rule-level cycles are conservative
where a column mixes formulas that never share a row.

### Dirty ranges, jobs and cancellation

Dirty state is a **range per rule**, never a flag per cell:

- an aggregate instance being rebuilt: a block cursor (blocks before it
  hold new partials, deltas below the cursor apply, above it wait for the
  job);
- a chain: the checkpoint frontier plus the rows edited above it (marks);
- a rule made stale by a scalar: the whole rule (its column's readers
  rebuild);
- the structural read: a byte cursor.

**Jobs.** Work above a budget (`RTX_WB_INLINE_WORK` row evaluations,
256k) is a job: an aggregate rebuild over a big table (a new formula, a
changed scalar input, a retype), a chain recompute, and the structural
read (open, and the reparse after a structural edit — W2's deferred
reparse, now a job). Jobs are stepped from the host's idle loop in
slices of `RTX_WB_SLICE_MS` (8 ms), each ending at a block boundary; the
keystroke path never runs one. Each job carries the edit generation it
was planned for and checks it at every block: a row edit below the
cursor is applied as a delta to what the job built, one above it is read
when the job gets there; a change to the job's own inputs (the scalar
that started it) restarts it; nothing valid is thrown away. **Cancel**:
Esc while the status says "recalculating", or **Workbook: Cancel
Recalculation**, drops the pending jobs; their nodes keep their last
values, marked stale, until the next edit or **Workbook: Recalculate**
plans them again. An explicit query (`rtx_wb_eval`, the dump, `--batch`)
settles every job first.

Within the phase-1 embedded engine jobs are cooperative (the UI thread,
bounded slices), because the model they read is the one edits mutate;
phase 2's external tables read immutable page-store bytes and can run
the block scans as `@parallel` dest-live arms that publish at `@stage`,
like find.

**Priority.** Within any recalc, what is visible comes first: the view's
cells are evaluated on demand at paint (they never wait for a job), the
status line's cell likewise, and a job whose result a visible cell or the
status line is waiting for runs before the others (then in topological
order). A chain job first reaches the view's rows.

**Stale display.** A value that is not yet recomputed paints its last
value, dimmed, after a stale marker `≈` (the theme entry
`comment.stale.workbook`, `rtx_theme_stale`), in both frontends; the
status line says `recalculating 43%` while jobs run. A value that has no
last value yet (a new aggregate over a table still being read) paints
`≈…`.

### Memory budget

| Per | Bytes | What |
|---|---|---|
| row | 20 | offset in block 4, length 4, row id 4, row id → (block, slot) 8 |
| row, per indexed column | ~24 | a key index entry |
| cell | 0 | no node, no value |
| block (256 rows), per aggregate | 0–4 | the stop-row count (only once a row errs) |
| block, per `min` / `max` | 24 | its best row |
| block, per aggregate over a chain | ~0.5–1 KB | the full partial (exact sums) |
| block, per chain | 16 × K × chain columns | the carried values |
| aggregate instance | ~1 KB | the root partial (the float superaccumulator is 568 B) |
| workbook | ~320 KB | the view cache (4096 cells) |

So a table costs its bytes plus ~20 bytes a row, plus per aggregate a
few bytes per 256 rows: 10M rows is ~200 MB of index. W2 kept ~3.5 KB a
row.

### Relative and absolute references

**Relative rows** (in a row formula): `@col[-1]` is the previous row's
`col`, `@col[+2]` two rows down; `k` is a whole number, `|k| ≤ 64`
(`RTX_WB_REL_MAX`). Outside the table the value is `null`, so a running
total reads `=coalesce(@total[-1], 0) + @amount`. A column that reads
itself through previous rows (directly or through other columns) is a
**chain**; its blocks carry checkpoints. An edit at row r makes the chain
dirty from r; recomputation is lazy — as far as the view or an aggregate
over the chain needs — and stops at the first block whose end state
equals its old checkpoint (a running `max` converges; a running sum does
not). Next-row references (`@x[+k]`) are allowed when they do not feed a
chain back into itself.

**Fixed rows: anchors (owner decision).** A row is named by an
**anchor**, never by its position. `{#q3}` at the start of a row's
first cell names that row; the tag is not part of the cell's value (`|
{#q3} Q3 total | 120 |` has the value `Q3 total`). `#first` and `#last`
are implicit. References:

| Written | Reads |
|---|---|
| `T#q3.amount` | `amount` of the row anchored `q3` |
| `T#q3[+1].amount`, `T#first[+2].x`, `T#last[-1].x` | k rows from the anchor, `|k| ≤ 64` (`RTX_WB_REL_MAX`); outside the table: `null` |
| `sum(T[#q1 .. #q3].amount)`, any aggregate / `where` over `T[#a .. #b]` | the rows from `#a` to `#b`, both included |

A missing anchor is `#ref(anchor q3 not found in T)`; one named on two
rows is `#dup(anchor q3 names rows 4 and 9 of T)` (the first two rows,
in table order). A range whose end is missing is `#ref`; a range whose
ends are reversed (`T[#q3 .. #q1]` with `q3` below `q1`) is
`#ref(range #q3 .. #q1 is reversed (rows 9 .. 2 of T))` — never a
silently empty sum. Key lookups (`T[id = 42].col`) are unchanged and stay
the way to name a row by its data.

Anchors move with their rows: an insert, a delete or a move anywhere
never changes what `T#q3` names, so nothing is ever rewritten and bytes
stay exactly what the user typed. **Workbook: Rename** (F2) on an anchor
renames the tag and every `T#name` / `#name` range end in this file, one
undo step (references from other workbooks, phase 2, are not rewritten:
they read `#ref` until updated). In Rich view the tag stays visible,
painted in its own theme entry (`entity.name.anchor.workbook`,
`rtx_theme_anchor`: an accent, no background) in both frontends.
**Workbook: Anchor Row** inserts `{#name}` at the start of the caret row's
first cell (the name made from the row's key or first cell, unique in the
table), one undo step, caret on the name; **Workbook: Copy Reference to
Row** copies `T#name.col` for the caret cell when its row has an anchor,
and otherwise says "row has no anchor — use Anchor Row" and does nothing.
No anchor is ever inserted automatically.

A bare position (`T.amount[5]`) is a bind-time error: `#ref(T.amount[n]:
rows are named by anchors, not positions (add {#name} to the row's first
cell, then write T#name.col))`. Positions were rejected because an insert
above row 5 silently changes what `T.col[5]` means; the two ways around
that — rewriting every reference on each insert (Excel), or commands that
do it on request — either make bytes stop being what the user wrote or
leave references from other files drifting. An anchor is in the bytes,
travels with its row through any edit, and reads the same from every
file.

### Cross-workbook references (phase 2)

`uses prior = "../q2.wb.md"` — a ```` ```uses ```` fence of `name =
"path"` lines (paths relative to this file). The other workbook opens as
its own `RtxDoc` with its own engine, read-only from here, namespaced:
`prior.eu_total`, `prior.Sheet.name`, and its tables for aggregates and
lookups (`sum(prior.Sales.amount)`).

- **Graph**: one import node per referenced name or table of the other
  file; a rule-level edge from the reader to it. The other engine owns
  its own graph; nothing crosses files per cell.
- **Change detection**: when the other file is open in the workspace,
  its engine publishes a stamp (the edit generation plus a publish
  counter that moves when any of its published values changes); the
  import node compares stamps at sync / idle. When it is not open, the
  identity triple (mtime, size, inode — the save stamp) is checked at
  focus and save, as for external tables; a changed file is re-read by a
  job.
- **Cycles across files**: the import graph must be acyclic. Opening a
  workbook that is already on the current import stack makes that
  `uses` line `#cycle(a.wb.md → b.wb.md → a.wb.md)`; every reference
  through it carries the error. Name-level cross-file cycles cannot then
  exist.
- **Stale propagation**: while the other engine has jobs (or is reading
  the file), its values are stale; import nodes are stale; readers paint
  dimmed. The idle loop steps every engine in the workspace, hidden
  imports included.
- **Anchors** across files: `prior.T#q3.amount`,
  `sum(prior.T[#q1 .. #q3].amount)`; the other engine's anchor index
  answers, so they follow inserts there too.
- **Rename** in the other file breaks this file's references
  (`#ref(prior has no name x)`, `#ref(anchor q3 not found in prior.T)`);
  a cross-file rename is a workspace transaction (phase 3).

### External tables and the on-disk block index (phase 2)

A ```` ```table ```` fence with `source:` binds a CSV / TSV file as its
own `RtxDoc` (page store, no copy). Its rows are the same blocks, built by
the structural read over the CSV bytes — as a `@parallel` dest-live arm
over 2 MiB chunks with the two-state quote trick (§5) — and persisted in
the cache dir (`RTXI`: identity triple, parser hash, then the blocks and,
keyed by formula hash, the block partials). Above 10M rows the per-row
offsets are dropped: a block keeps its first byte and row count, and a
row is found by scanning the block's bytes (≤ 64 KiB, one page read).
External tables have no formula cells; computed columns declared in the
fence are rules with no per-row text. Everything above the block — the
rule graph, shapes, jobs, stale display — is the embedded engine's.

### Migration from W2

- **Kept**: the value model, exact arithmetic (`wb_num`), the lexer,
  parser and most of the binder, the AST evaluator, the W2 partials
  (`WAcc`, stop rows, `min` / `max` bests, group maps), the fast paths'
  classification (cell, line, prose, rows, reparse), the hooks and the
  API, rename, the live annotation.
- **Replaced**: `WNode` per cell and per column (gone), per-cell values
  (recomputed from bytes; a bounded view cache), W2's `ov` old values
  (the pre-edit hook's old row bytes, evaluated in an "old" mode), MAs
  per aggregate occurrence per cell (aggregate nodes with instances per
  parameter tuple), leaves (blocks with stable ids and per-block row
  arrays), `regraph` over cells (Tarjan over rules, always), the idle
  reparse (the structural-read job).
- **Semantics kept**: every W0 / W2 fixture value; errors name the cell
  that reads them; cycles with W0's paths (conservative at rule level as
  above); bit-identical incremental and from-scratch results.

### Phases

| Phase | Scope | Exit criteria |
|---|---|---|
| **1 — embedded** (built: "W1 as built") | the rule graph with shapes; no per-cell state; blocks with row ids; block partials reusing W2's MAs and exact arithmetic; view-first evaluation; stamped cooperative jobs with cancel; stale display in both frontends; relative rows with checkpoints and convergence; anchors (`{#name}`, `T#name.col`, anchor ranges); dirty ranges; key indexes on both sides of a lookup; every W0 / W2 feature | `wb_smoke`, `wb_prop_smoke` (with relative / absolute references, cancel mid-job, edits during a job) and `wb_scale_smoke` bit-identical to scratch after every checked step; chain tests (a running total over 100k rows, an edit in the middle, the view at the top vs the bottom, convergence counters); anchors under insert / delete / move / rename; a PTY test of the stale display; priority (visible cells settle before the job finishes); `wb_scale_perf` at 100k–10M: first screen < 100 ms, typing never blocked, ~100 B/row |
| **2 — external and imported** (built: "W1 phase 2 as built", without `RTXV` and the 1e8 targets) | `table` fences over CSV / TSV; the on-disk block index (`RTXI`) and persisted partials; parallel block scans; `uses` imports; persisted last values (`RTXV`) so a reopen paints at once | 1e8-row CSV: build ≥ 1 GB/s on 8 cores, reopen ≤ 50 ms, RSS ≤ 64 MB above the page cache; a cross-file edit reaches the reader in one idle turn; import cycles reported |
| **3 — relations and views** | filter / sort views (counted selection trees), zone maps, `where k = @k` compiled to a group automatically, an LSM key index for 1e9 keys, the formula bar, cross-file rename | W3–W5 of §8 |

## W1 as built

Phase 1 of [the lazy engine](#w1-the-lazy-engine-design) replaced W2's
engine for embedded tables (`core/wb.ccs`; the design above holds, with
the anchors decision). Every W0 / W2 fixture value is unchanged.

**What is stored.** Per table: blocks of ~256 rows (split above 512,
merged below 64) with stable block ids, each block's rows as relative
offset / length / stable row id, and a row id → (block, slot) map —
**~21 bytes a row** (210 MB at 10M rows). Per aggregate instance: the
root partial (W2's `WAcc` with the exact sums) and per block only what
it needs (stop counts, `min` / `max` bests, and for aggregates over a
running chain the whole block partial plus a segment tree for anchor
ranges). Per running chain: the carried values (the last K rows of every
chain column) at each block's end, and the frontier. Key indexes for
the columns an equality lookup reads (built on the first probe, kept by
edits); the anchor index (name → row id). No node and no value per cell:
a cell's value is recomputed from its row's bytes when something needs
it; a bounded view cache (4096 entries) keeps what was painted, checked
against a value generation. The old value a delta subtracts comes from
the edited row's old bytes, captured by the document's new `pre` hook
before the tree write and evaluated in an "old" mode (row positions,
anchors and key indexes still old; rows after the edit read through the
edit's delta).

**The rule graph.** Nodes: names, rules (one per distinct formula text
per column — a million `=@a * @b` cells are one rule), columns (hubs
holding their rules), aggregates (instances per outer-row parameter
tuple, made on first read), anchored cells. Edges carry the shapes of
the design; Tarjan over rules marks hard cycles (`#cycle(path)`, W0's
paths, recomputed when rows move) and finds chains (a column reading
itself through earlier rows); a same-row loop is found per row while
evaluating. A node is evaluated once per heap round (a re-queue inside
the round is a cycle), and staleness does not travel along edges inside
one component.

**Jobs.** An aggregate's build, a chain recompute past the view, a group
key's min / max rescan over a big table, and the structural read (open,
and the reparse after a header / fence / caption edit, in a workbook of
`RTX_WB_DEFER_BYTES` or more) are jobs. The inline share of an edit is
`RTX_WB_INLINE_WORK` work units (row × instance evaluations, 256k) and
one slice of time; the rest runs from the host's idle loop in
`RTX_WB_SLICE_MS` (8 ms) slices (`rtx_wb_idle`; `rtx_wb_due_ms` says
when), the aggregates the view or a painted stale name waits on first.
Esc while busy, or **Workbook: Cancel Recalculation**, cancels
(`rtx_wb_cancel`): half-built instances are rebuilt when the next edit or
Recalculate plans them; a chain resumes from its frontier (a block whose
checkpoint a job rewrote marks the next, so a later convergence cannot
skip a checkpoint made from the old state). A query (`rtx_wb_eval`, the
dump, `rtx_wb_settle`) runs every job to the end first.

**Stale display.** A value not yet recomputed paints its last value after
`≈`, in `rtx_theme_stale` (`comment.stale.workbook`), in both frontends
(the derived hook's `at` returns 2; `RtxHintVis.stale`); `≈…` when there
is no last value. Names carry their last values across a rebuild, so a
structural edit in a big workbook shows the old values stale, not blank.
The status line starts with `recalculating 43%` (or `recalculation
cancelled (stale values)`) before the element at the caret.

**Running totals.** `=coalesce(@run[-1], 0) + @x` (any `@col[-k]`,
`|k| ≤ 64`, reading its own column directly or through others): the
view's rows are exact at once when within `RTX_WB_VIEW_CHAIN` (32768)
rows of the frontier, else stale until the chain job passes; a name
reading past that reach (`T#last.run`) is stale until then too. An edit
marks its block; the job re-walks from the frontier and stops where a
block's end state equals its checkpoint, skipping to the next mark (100k
rows with a reset every 1000: an edit walks 1340 rows and skips 387
blocks).

**Anchors** as in [Relative and absolute references](#relative-and-absolute-references):
`{#name}` visible in Rich and Source (`rtx_theme_anchor`, through the
derived hook's new `style` runs), `T#name.col`, offsets, `#first` /
`#last`, `T[#a .. #b]` for every aggregate, `#ref` / `#dup` texts,
rename (F2) as one undo step, **Workbook: Anchor Row** and **Copy
Reference to Row**; a bare position is a bind-time error.

**Numbers.** The contract, checked by `wb_smoke numops` against
`tests/wb_num_gen.py` (Python ints / Fractions / doubles, 1532 cases,
floats bit for bit):

| | Rule |
|---|---|
| int, dec | exact; `+ - %` at the larger scale, `*` at the sum of the scales; past 12 fraction digits rounded **once**, half away from zero, to 12; past int64: `#num` |
| `/` | float division of the two sides converted to the nearest double |
| float | IEEE for `+ - * /`; `%` is C `fmod` (exact: `1e20 % 3` is 2); int / dec `%` is the truncated remainder (the dividend's sign) |
| comparisons | exact for int / dec; a float against anything compares as doubles; NaN is unordered: `=` false, `!=` true, `< <= > >=` false; `where` follows (a NaN row is not selected) |
| `sum`, `avg` | every row exact, rounded once (with a float: to the nearest double, ties to even); NaN in it, or +inf with -inf: NaN; `[1e308, 1e308, -1e308]` sums to 1e308 |
| `min`, `max` | NaN in the column (or among `min(a, b)`'s arguments) makes the result NaN |
| literals | `-9223372036854775808` is an int (cell or formula); `9223372036854775808` alone is out of range; a column's type rescales its numbers (dec to its largest scale, all to float if one is float) |
| dates | `date ± int` past ±25 million years is `#num` (no overflow) |

`round(x, n)` of a float rounds the double `x·10ⁿ` half away from zero
and divides back; of an int / dec it is exact. The wb tests run clean
under ASan + UBSan (`make` recipe: build `wb_*` with `-fsanitize=undefined,address`).

**API added** (`core/wb.cch`): `rtx_wb_settle`, `rtx_wb_busy`,
`rtx_wb_cancel`, `rtx_wb_progress`, `rtx_wb_job_limits`,
`rtx_wb_anchor_row`, `rtx_wb_row_ref`; stats `rules`, `insts`, `jobs`,
`kx_probe` / `kx_builds`, `range_q`, `chain_*`, `job_rows`, `cancels`,
`slices`, `mem_index`, `mem_partials`. `RtxDerived` gained `pre` and
`style`. Test knobs (environment, read once): `RTX_WB_INLINE_WORK`,
`RTX_WB_SLICE_MS`, `RTX_WB_SLICE_ROWS`; `RTX_WB_CHECK=1` checks the
model's invariants after every sync (debug).

**Tests.**

- `wb_smoke`: the fixtures (plus `anchors`: every anchored form and its
  errors), counters in rule-graph terms, the job-based deferred read,
  `numops`, the property loop (reports the edit and the bytes before a
  mismatch; `WB_STEP_TRACE=1` records every step for a hang).
- `wb_w1_smoke` (new): the anchors property loop over three seeds with
  and without forced jobs (idle slices, cancels, edits on top of pending
  work; keyed lookups into a second table; anchors added / removed /
  renamed / duplicated; row moves); the 100k-row running total (top
  exact once read, bottom stale then settled, a top edit walks every row,
  an edit above a reset converges); priority; a refused rename edits
  nothing (bytes, undo, a redo branch); edit / edit during a job /
  cancel / edit / undo, then exact.
- `wb_prop_smoke`, `wb_scale_smoke`: unchanged scenarios, green
  bit-identical to scratch (the scale smoke settles between steps).
- `tui_pty_test.py`: `wb_stale` (dimmed `≈` last value, `recalculating`,
  Esc cancels, the next edit recomputes), `wb_anchor` (tag visible in
  Rich and Source in its own colour, the two commands, one undo),
  `wb_deferred` (the read job).
- `tests/wb_dump`: dump / eval / edit-replay tool.

**Perf** (`wb_scale_perf`, release, a shared 4-core host, 15 GB; the
generated workbook of W2 at scale — 1000 + 100 + 10 aggregates at 100k
and 1M rows, 100 + 100 + 10 above; one sample for open / read / settle,
p50 of 20 for edits; the chain tier is a running total over the same
row count):

| Op | 100k | 1M | 2M | 5M | 10M |
|---|---|---|---|---|---|
| file | 8.1 MB | 82 MB | 165 MB | 414 MB | 829 MB |
| open: text on screen | 2.3 ms | 0.1 ms | 0.5 ms | 0.1 ms | 0.15 ms |
| read (job): the view's values | 99 ms | 0.76 s | 1.57 s | 3.9 s | 7.9 s |
| settle: every aggregate built | 7.0 s | 72 s | 26 s | 63 s | 140 s |
| RSS after settle (index / partials) | 26 MB (2.3 / 3.8) | 45 MB (20 / 38) | 70 MB (42 / 7) | 111 MB (100 / 23) | 202 MB (210 / 35); peak 1.55 GB |
| cell edit under 1 / 100 / 1000 aggregates | 0.05 / 0.10 / 0.98 ms | 0.07 / 0.09 / 0.90 ms | 0.03 / 0.05 / 0.12 ms | 0.03 / 0.06 / 0.14 ms | 0.06 / 0.11 / 0.18 ms |
| row insert / delete | 0.65 ms | 0.66 ms | 8.5 ms (a group key's max: a job) | 0.18 ms | 0.84 ms |
| param flip: keystroke / settled | 8.5 ms / 41 ms | 8.6 ms / 0.53 s | 8.2 ms / 1.0 s | 8.2 ms / 2.8 s | 8.2 ms / 5.5 s |
| structural read (header edit) | 89 ms | 0.81 s | 1.6 s | 3.9 s | 7.9 s |
| keystroke in a Rich cell / at a formula's end | 10.3 / 10.1 ms | 10.1 / 10.2 ms | | | |
| PageDown in Rich | 8.7 ms | 8.7 ms | | | |
| running total: read / settle | 49 ms / 33 ms | 0.29 s / 0.66 s | 0.57 s / 1.3 s | 1.45 s / 3.3 s | 3.0 s / 10.1 s |
| running total, edit at the top: keystroke / row below exact / settled | 8.3 ms / 0.02 ms / 33 ms | 8.2 ms / 0.015 ms / 0.57 s | 8.6 ms / 0.02 ms / 1.1 s | 8.3 ms / 0.014 ms / 2.9 s | 8.4 ms / 0.015 ms / 5.7 s |

Against the targets: memory is ~21 B a row of index (10M rows: 210 MB
index, 1.55 GB peak with the file's pages); typing never waits (a cell
edit is under 1 ms at every size, a param flip or a top-of-chain edit
returns within one 8 ms slice and the rest is a job); the first screen
of **text** is immediate, but the first screen of **values** waits for
the structural read — single-threaded at ~100 MB/s (0.76 s at 1M, 7.9 s
at 10M), so the "< 100 ms" target is met to ~100k rows only. Reading
the rows is the cost; the phase-2 persistent block index (`RTXI`) and
the parallel read make a reopen immediate. Building a thousand
aggregates over a million rows is a minute of background work (65 ns a
row per aggregate); the view's aggregates build first.

**Limits and open items.**

- A key index answers the build side of a lookup (`T[id = x]`); the
  reader side was built in phase 2 (see
  [W1 phase 2 as built](#w1-phase-2-as-built)).
- A group key losing its `min` / `max` rescanned the table; phase 2
  keeps per-key block bests (a block rescan).
- A table whose rows read their neighbours (`@x[-1]`) drops its key
  indexes on an edit (rebuilt at the next probe).
- References from other workbooks (phase 2) are not rewritten by a
  rename; `Copy Reference to Row` puts the text in the editor's clip
  (the OS clipboard sync follows the Copy command only).
- The structural read is a whole-workbook pass; values of a big
  workbook appear when it ends (above). Phase 2's persisted index skips
  it on a reopen.

## W1 phase 2 as built

Phase 2 adds, on the phase-1 engine (`core/wb.ccs`, one TU): a persisted
index so a reopen paints values without reading table bodies, a parallel
structural read, reader-side key indexes, group `min` / `max` by block,
`uses` imports between workbooks, and read-only tables bound to CSV / TSV
files. Every W0 / W2 / W1 fixture value is unchanged, and every property
test compares with a from-scratch model bit for bit.

### The persisted index (`.wbi`)

- **Where**: `<cache>/w/<fnv(realpath)>.wbi`. The workspace points the
  cache at the Safe dir (`rtx_safe_dir`), so the TUI and the UI use it;
  tests and tools opt in with `rtx_wb_set_cache_dir`; `RTX_WB_INDEX=0`
  turns it off.
- **Identity**: the workbook file's `dev`, `ino`, `size`, `mtime` ns and
  `ctime` ns, taken when the bytes were read, and `core/sindex`'s trust
  rule: a stamp within the file system's granularity of the read
  (racily clean) is not trusted, so such a file is neither written nor
  loaded. The document must be clean (`RtxDoc_dirty` 0, same length).
- **Format**: magic `RTXWBI1`, version (`WBI_VER` 4), the caps it was built
  under (row cap, column cap, block size), the identity, then one section
  per table in file order (name, body start / end, column stats, rules
  with their cell counts, anchors, blocks: live flag, first byte,
  row lengths / relative offsets / row ids, compact when regular), then
  the post part: aggregate instances by a stable key (owner label, source
  hash, ordinal) with their root and block partials, complete chain
  checkpoints, built key indexes. Every record has a length, so an unknown
  one is skipped. The last 8 bytes are a checksum (four-lane 64-bit word
  hash) over the rest.
- **Writing**: built in memory, handed to one writer thread, written to
  a temp file, `fsync`ed and renamed; a newer job for the same path drops
  an older one. Levels: 1 positions (the read ended), 2 with the partials
  (no jobs), 3 with every chain complete (after a settle). An index is
  written when it would say more than the last one for this model.
- **Loading**: `RtxDoc_from_path` → `rtx_wb_attach_path`: identity,
  length, magic, version, checksum, caps, path — any mismatch is ignored
  (`rtx_wb_index_why`: no file, bad, version, checksum, path, identity,
  racy, caps, length, mismatch) and the read happens as before. On a
  match the read goes over every line outside table bodies (the text is
  parsed as usual) and, at each table, installs its section and jumps to
  the body's end: no body byte is read. After the bind, the partials,
  chains and key indexes are restored by key; an aggregate whose key or
  shape does not match is simply built (a job). A section that does not
  parse drops the index and restarts the read without it.
- **Lazy validation**: nothing in the body is re-checked at load; the
  identity is the check. The first edit after a load goes through the
  normal paths: `wb_p2_smoke persist` runs 300 random edits after a load
  and compares with a from-scratch model after each checked step.
- **Not restored**: aggregates that read another workbook (`uses`),
  chains and key indexes of a workbook with imports, group per-block
  bests (rebuilt lazily). The index is not re-stamped after a save (the
  next settle writes a new one).

### The parallel structural read

Inside a big table the structural read pre-reads a window of the file
in lanes (`@parallel for` over `rtx_par_pool_up` workers, reading with
`RtxPieceTree_read_uncached`, which is lane-safe): each lane cuts its
byte range into segments of pipe rows with their lengths, cell counts,
formula texts and anchors, looking one line past its end for a separator
row. The serial state machine then takes a whole segment at the cursor
in one step; a header- or separator-shaped line, a long line, or
anything unusual goes back to the state machine, so the model is the
serial read's exactly (`rtx_wb_model_hash`, compared in `wb_p2_smoke par`
for 2, 3, 4 and 7 lanes and five window sizes against one lane).

The windows are filled by a background dest-live arm
(`A->h = @parallel { ... } !>`, the `proj.ccs` pattern): a ring of four
windows of 2 MiB a lane runs ahead of the read; the read takes them in
order and ends its idle slice when the arm is behind (typing never waits
for lanes). The document's `pre` hook (before every tree write) cancels
and joins the arm and drops its windows, so no window ever describes
bytes that changed; the read re-kicks it at its next table row. `RTX_PARALLEL_INLINE=1` runs the windows
synchronously (the tests run both ways). `rtx_wb_read_lanes(n, chunk)`
sets the lanes (0 auto, 1 serial).

### Reader-side key indexes

`=K[id = @cust].w + @x` over a reader table R: on the first edit of K
that needs it, R gets an index probe key → R row ids (one walk of R).
An edit of a non-key cell of a K row then takes only the R rows whose
probe equals that row's key as row deltas (old side through the captured
K row, new side after), so `sum(R.v)` updates by delta instead of
rebuilding as a job. Eligible: a probe made of the row's literal cells,
literals and scalar operators; a literal key column in another table; no
relative readers; at most a quarter of R's rows touched (else the old
wholesale path). Edits of R's probe or rule cells, or R's rows moving,
drop the index (rebuilt on the next need).

### Group `min` / `max` by block

For `group T by k { lo: min(x), hi: max(x) }`, per key and output the
best row of each block the key has rows in. A key losing its extreme
rescans only the block(s) whose best left — every key waiting on a block
in one pass — and takes the best of its blocks' bests: a block, not a
walk of the table, and never a rebuild job. Splits and merges rescan the
pieces. **Trade-off**: the maps cost a slot per (key, block, output);
past `max(64k, rows)` slots an instance drops them and falls back to the
phase-1 rescan (still correct, a job on a big table). They are not
persisted: an instance restored from the index starts without them and
uses the rescan until its next build.

### `uses` imports

````
```uses
prior = "q2.wb.md"
```
````

`name = "path"` lines, relative to the workbook, name other `*.wb.md`
files, read-only and namespaced: `prior.total`, `prior.Sheet.total`,
`prior.T#q3.amount`, `prior.T#q3[+1].amount`,
`sum(prior.T[#q1 .. #q3].amount)`, `count(prior.T where x > 1)`.

- **Graph**: each distinct reference is an import node (a scalar input
  of its readers, rule-level edges). The other workbook keeps its own
  engine and graph; the reference's text is evaluated there.
- **Which engine**: a workbook open anywhere in the process (a pane —
  its live bytes, unsaved edits included) is found by realpath in a
  registry; otherwise the importer opens a private read-only copy, whose
  file identity is checked from the idle loop (`rtx_wb_uses_poll_ms`,
  default 1 s) and re-read when it changes. Private copies' jobs run
  from the importer's idle slices.
- **Change detection**: the other engine's stamp (value and model
  generations, busy) is compared at every query, sync and idle slice;
  import nodes of a moved alias re-read, cut off at equal values.
- **Stale**: while the other engine has jobs its values are stale, and
  so are the imports and everything reading them (the same `≈` display;
  `tui_pty_test wb_uses` edits one pane and watches the other go stale,
  then fresh).
- **Errors**: a missing or unreadable file, a non-workbook, a duplicate
  alias → `#ref(...)` on every reference through it; an unknown name →
  `#ref(prior has no name x)`. **Cycles**: nested opens form an attach
  stack and live engines' `uses` are walked before one is taken; a
  workbook reachable from itself makes that alias `#cycle(...)` with the
  full paths.

### External tables (CSV / TSV)

````
```table Sales
source: data/sales.csv        # relative to the workbook
format: csv; header: 1        # csv | tsv; delim: ;  quote: '  header: 0
columns:
  id: int <- "Order ID"       # bind by header name ...
  region: text
  amount: float <- @3         # ... or by position
```
````

- **Records**: RFC 4180 — quoted fields with delimiters, doubled quotes
  and newlines inside, CRLF or LF, a missing final newline; blank lines
  are skipped. A field is a literal (a leading `=` is text, never a
  formula). Types: declared, else inferred from the column's values as
  for embedded tables.
- **Header check**: a declared column the header lacks is
  `#schema(Nope: data/sales.csv has no column Nope)` in every cell of
  it; without `columns:` the header names them (`c1`, … with
  `header: 0`).
- **The file** is its own read-only `RtxDoc` (page store, no copy); its
  rows are the same blocks as an embedded table's (positions into the
  file), so aggregates, lookups, key indexes, groups and the stale
  display are the embedded engine's. A file of `RTX_WB_DEFER_BYTES`
  (1 MiB) or more is read by the read job; a smaller one at open.
- **Changes on disk**: the file's identity is checked from the idle loop
  (as for private imports); a change marks the model for a rebuild (the
  file is read again; values stay visible, stale, meanwhile).
- **Persisted**: the index's section for the table carries the file's
  realpath and identity (the same trust rule); a reopen installs the
  file's block positions, partials and key indexes when both identities
  match, else reads the file. A missing file is recorded as missing.
- **Errors**: a missing / unreadable file is `#ref(Sales: cannot read
  data/sales.csv)` on every reference.

### Tests

- `wb_p2_smoke` (new; smoke, asan, tsan lists; `./bin/wb_p2_smoke
  [steps] [section]`): `persist` (write / reopen round trip; stale,
  corrupt, truncated, version, identity, racily clean refused; 300 random
  edits after a load bit-identical to scratch), `par` (model hash equal
  to the serial read for 6 seeds × 2/3/4/7 lanes × 5 window sizes; jobs
  and edits during the arm), `lookups` and `gminmax` (property loops:
  no aggregate rebuild on a K.w edit / a value edit, bit-identical to
  scratch every 10 steps), `uses` (private, changed on disk, live pane
  and its undo, stale then fresh, missing, unknown name, cycles private
  and live with full paths), `csv` (quoting, CRLF, blank lines, `=` as
  text, tsv, `#schema`, a missing file, a file that grows, the index
  round trip and a changed file, 200k quoted records).
- `tui_pty_test.py wb_uses`: two panes, an edit in the imported workbook
  reaches the reader (stale mark, then the new value).
- `wb_scale_perf`: `read_serial` vs `read` (lanes), `index_write_wait`,
  `reopen`, `reopen_settle`; `--csv rows` for the CSV tier.
- Pins (`scripts/wb_perf_pins.sh`, `testdata/perf/wb_pins.env`): 100k
  `read_serial`, `reopen` (27 ms measured: 1110 aggregates restored) and
  `reopen_settle` added to the W1 set.

### Perf

Embedded tables (`wb_scale_perf 1000000 10000000`, the generated
workbook of "W1 as built" with 1000 + 100 + 10 aggregates at both sizes;
one sample each):

| Op | 1M (82 MB) | 10M (829 MB) |
|---|---|---|
| open: text on screen | 0.2 ms | 13 ms |
| cold read, one lane (phase 1) | 1.32 s | 19.5 s |
| cold read, lanes + arm (phase 2) | 1.54 s | 20.2 s |
| settle: every aggregate built (1110) | 153 s | 30 min |
| index file / writer wait after the settle | 13 MB / 0 ms | 122 MB / 1.6 s |
| **reopen from the index → first values** (all 1110 aggregates restored, no job) | **136 ms** | **1.10 s** |
| of which reading + checking the index | 21 ms | 233 ms |
| reopen settle: builds / job rows | 0 / 0 | 0 / 0 |
| cell edit after, under 1 / 100 / 1000 aggregates | 0.05 / 0.10 / 0.98 ms | 0.15 / 0.18 / 2.0 ms |

The parallel read gave nothing here: the host ran other agents' builds
and tests (load average 16–25 on its cores), so lanes waited for CPU
(`lanes_ms` ≈ the serial time). On an idle 4-core machine the lanes are
the only way the cold read gets faster; the reopen is what this phase
makes fast, and it holds under load: a 10M-row workbook that took 30
minutes to settle paints every aggregate's value 1.1 s after open,
building nothing.

CSV tier (`wb_scale_perf --csv`, a Sales table over a generated
`data/sales.csv`: 5 columns, a quoted field every 8th record, ~33 B a
record; 5 aggregates — `sum`, a `where`, `count`, `sum` of an int
column, `max`; with and without a keyed lookup `Sales[id = 12345]`,
which needs a key index over 1M / 10M distinct ids):

| Op | 1M (31 MB) | 10M (335 MB) | 1M + lookup | 10M + lookup |
|---|---|---|---|---|
| open: text on screen | 3.1 ms | 0.5 ms | 3.1 ms | 0.5 ms |
| cold: read job + 5 aggregates (+ key index) → first values | 0.67 s | 6.1 s | 3.2 s | 31.5 s |
| RSS after (index) | 45 MB (20 MB) | 229 MB (210 MB) | 210 MB (116 MB) | 1.32 GB (978 MB) |
| index file | 2.1 MB | 20.7 MB | 34.5 MB | 345 MB |
| reopen from the index → first values | 31 ms | 0.53 s | 0.49 s | 4.3 s |
| of which reading + checking the index | 4.7 ms | 59 ms | 33 ms | 0.71 s |
| RSS of the reopened model | 8 MB | 189 MB | 57 MB | 956 MB |
| reopen: aggregate builds / job rows | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

(A shared host with a load average of 16–25 from other jobs: absolute
times are noisy; ratios are the point.)

### Limits and open items

- **Cold first values are not < 100 ms at 10M rows.** The parallel read
  helps only with idle cores (on the loaded host it measured no faster
  than one lane); a CSV file's records are read by one thread (~50 MB/s
  with 5 aggregates building in the same job). The persisted index is
  what makes a reopen fast.
- **Key indexes are big**: ~100 B a distinct key (a 10M-key index is
  ~1 GB in memory, 345 MB on disk) and restoring one is most of a
  reopen's cost (4.3 s of a 10M reopen with the lookup; 0.53 s without).
  A compact or on-disk key index is phase 3 (the LSM item).
- Reopen without key indexes still installs every block's row arrays
  (~0.5 s at 10M rows); per-block lazy install would make it constant.
- Private imports and CSV files are checked for changes once a second
  from the idle loop (not with inotify); a change rebuilds the whole
  importing model.
- Aggregates over an imported table are evaluated in the other engine
  (a scan there when it has no instance for them); they are not
  persisted in the importer's index.
- Cross-file rename is not a workspace transaction (a rename in the
  imported file breaks references: `#ref(prior has no name x)`).
- The index is written only for a clean, trusted file; a save does not
  re-stamp it (the next settle writes a new one).
- CSV tables are read-only; values are never written into a CSV.
- Settling 1110 aggregates over 10M embedded rows peaked at 7.2 GB RSS
  (5.9 GB after; partials 287 MB, index 210 MB) on this run: the
  aggregate builds' scratch at that scale is not bounded yet (W1's
  10M tier used 210 aggregates).

## Locks

| Topic | Lock |
|---|---|
| Truth | Workbook text and source bytes. Values, indexes and views are derived and rebuildable; values never go into the file |
| References | Named tables, columns, rows and scalars; no A1; static edges; no volatile functions; `asof` is a param |
| Recalc | DAG over names; topological order; early cutoff; cycles are `#cycle(path)`; no iteration |
| Change unit | Row delta on a block index with stable row ids (embedded, W1). Invertible aggregates add/subtract; min/max rescan one block, then the blocks' bests |
| Index exception | A table **declared in a workbook** gets a whole-table index. Plain grid stays windowed (`core/grid.cch`) |
| Build | Dest-live Scan-table row; two-state parallel parse; publish at `@stage`; partial is `pending`, never final |
| Persistence | `.wbi` in the cache dir (the Safe dir): file identity (dev, ino, size, mtime / ctime ns; racily clean refused) + caps + a checksum; any mismatch or unknown version ignored (the read happens) |
| Edits | `replace` hook on the table doc; cancel+wait a live build, patch built leaves, re-kick |
| Arithmetic | `dec` exact (per-scale i128, a 256-bit total); `float` exact superaccumulator, rounded once (NaN / ±inf by count); results independent of edit history and row order |
| Non-goals | A1 grids, macros, volatile functions, collaborative OT, server engines, writing values into CSVs |

# Language friction (out-of-tree)

Use this file to record Concurrent-C limits that force awkward structure in
raytext (include lowering, sugar, arenas, etc.).

Recipes live in Concurrent-C `docs/cheatsheet.md` (Arenas: `@scratch` is
call-local; pass a named arena last to keep a product).

## Lazy queries have no honest signature (want: `!>` effect atoms on typeview)

`line_start` / `line_of` are logically queries but physically complete the
progressive index — they reach `read_at`. Neither signature is honest: a
Result infects every caret call site with unactionable errors (the failure's
scope is the document, not the call); a value invites the fallback lie —
`@errhandler { (void)e; return len; }` teleports the caret to EOF and records
nothing. raytext holds the line with `line_off_ok` by convention; nothing
checks that discharging handlers actually set it.

Want `!>` atoms in typeview grant lists. Function-only, UFCS-matched, gated
before resolution like every other grant; the demanded tier is verified on
the resolved body (single lowered unit makes reachability cheap). The type
stays general and open — the **application** declares the face and demands
the tier, because the composition knows what is safe, not the tool
(ownership at the call site, extended to effects):

```ccs
typedef @typeview Layout on RtxDoc {
    r: !>total len, has_sel;                        /* verified: no reachable discharge */
    r: !>lazy(line_off_ok) line_start, line_of;     /* discharging handlers must write the mark */
    r: read_at, scratch_span;                       /* plain fallible */
} *RtxDocLayout;
```

Rules:

- Tiers form a lattice (`total` ⊃ `lazy` ⊃ fallible); demands are monotone.
  Verification is per-view, application-scoped: the body must satisfy each
  demanding view in this build. The same library can verify in one app and
  fail in another — safe is relative to composition.
- Demand is discovery: satisfiable demands are limited to what the body
  actually maintains. A failed demand answers "what does this library really
  guarantee?" at compile time, pointed at the offending handler — not at
  caret-teleport time.
- Named lists demand (fail loudly: "`line_start` discharges CCError without
  writing `line_off_ok`"). Globs filter (standing policy): `!>*` = every
  fallible function — an audit face as a language object. A UFCS extension
  matching a face glob is held to its tier: open extension, closed guarantees.
- A `!>lazy` grant implies read access to the mark field; a face that can
  call the query but not see the mark reconstructs the silent-degrade bug
  inside the permission system.
- Degraded returns are documented clamps (scan frontier), never fabricated
  positions. Mutations refusing while the mark is down stays library policy,
  like `broken`.
- Indirect calls are fallible unless the pointer type carries a tier.
- `r: !>total destroy` makes "destroy cannot fail" checkable.

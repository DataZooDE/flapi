<!--
Target venues: project blog (canonical), syndicate to dev.to with
rel=canonical; submit to lobste.rs (security + c++ tags — playbook §7 notes
lobste.rs is higher-signal for this material) and optionally HN on a
different day than the Show HN.
Timing: Week 2-3 of the launch sequence (playbook §8) — after the flagship
post is live so this can link to it, before or alongside the Show HN so the
injection-safety claim in that thread has a canonical reference. Security
posts punch above their weight on HN/lobste.rs (playbook §7 item 3).
-->

# Making SQL injection structurally impossible — where we can prove it

flAPI builds REST endpoints and MCP tools out of SQL templates. Users send
parameters over HTTP; those parameters end up in queries against DuckDB.
That sentence should make you nervous — it made us nervous, and we wrote the
thing.

This post describes how we moved the hot path from "validated string
interpolation" to DuckDB prepared statements, and — more importantly —
exactly where the boundary of that guarantee sits. The claim we can defend
is narrower than the claim we would like to make, and we think the narrow
claim, stated precisely, is worth more than the broad one stated loosely.

## Where we started: validators and template discipline

A flAPI endpoint is a Mustache-templated SQL file plus a YAML definition.
The YAML declares each parameter with typed validators; the template
references parameters and the config chooses between raw interpolation
(`{{{ params.x }}}`) and HTML-escaped interpolation (`{{ params.x }}`):

```yaml
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1
        max: 1000000
```

```sql
SELECT c_custkey AS key, c_name AS name, c_acctbal AS balance
FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}
```

Historically, injection defense was two layers: the validator (an `id` that
isn't an integer in range never reaches the template) and template
discipline (documented rules about which brace form to use where). That is
a real defense — a typed, range-checked integer is hard to weaponize — but
it is a *policy* defense. Mustache knows nothing about SQL string literals,
quote-doubling, or comment syntax. Every validated value was still being
spliced into query text, and the safety argument was "the validator in
front of it is correct." We wanted a class of parameter for which the
safety argument is "the value never touches the SQL text at all."

## The pipeline: classify, rewrite, bind

The new path is three small components. None of them is clever, which is
the point.

**1. The classifier** (`src/sql_parameter_classifier.cpp`, 56 lines) looks
at a request field's validators and decides whether the field is *bindable*
— whether it maps to a SQL type DuckDB can take as a prepared-statement
parameter:

| Validator type | Bound as |
|---|---|
| `int`, `integer` | `BIGINT` (`duckdb_bind_int64`) |
| `number`, `float`, `double` | `DOUBLE` (`duckdb_bind_double`) |
| `boolean`, `bool` | `BOOLEAN` (`duckdb_bind_boolean`) |
| `date` | `DATE` (`duckdb_bind_date`) |
| `time` | `TIME` (`duckdb_bind_time`) |
| `uuid`, `string`, `email`, `enum` | `VARCHAR` (`duckdb_bind_varchar_length`) |

A field with no typed validator is not bindable. The first recognized
validator type wins, deterministically.

**2. The rewriter** (`src/prepared_template_rewriter.cpp`) scans the raw
template *before* Mustache renders it, and replaces qualifying `{{ params.x }}`
sites with `?` placeholders, recording a binding spec (field name, SQL
type, position) for each. A site qualifies only if all of these hold:

- it is a double-brace tag of the exact shape `{{ params.<name> }}`;
- `<name>` is a declared request field;
- the classifier says that field is bindable;
- the site is *not* inside a `{{#section}}` or `{{^section}}` block.

Everything else — triple-brace tags, `conn.*` and `cache.*` references,
undeclared names, section interiors — passes through byte-for-byte and is
rendered by Mustache exactly as before. Then Mustache renders the rewritten
template (expanding conditionals, connection properties, and any remaining
interpolation sites), and the result is SQL text containing `?` markers.

**3. The executor** (`src/query_executor.cpp`) hands that text to
`duckdb_prepare`, converts each raw parameter string to its declared type,
binds it with the type-specific C-API call, and runs
`duckdb_execute_prepared`. A value that fails conversion — `id=abc` for an
integer binding — is rejected as a 400 before execution. An absent optional
parameter binds SQL `NULL`.

For a bound site, injection is now a type error rather than a filtering
problem. There is no escaping step to get wrong because there is no
concatenation: `id` travels as an `int64_t` through
`duckdb_bind_int64(stmt, 1, v)`, and DuckDB's parser never sees it. The
statement's parse tree is fixed before the first user byte arrives.

One detail we're fond of: varchar values bind through
`duckdb_bind_varchar_length`, the length-aware variant, so embedded NUL
bytes survive as part of the bound value. The C-string variant would
silently truncate `alice\0' OR 1=1` to `alice` — which sounds *safer* until
you notice it means the value that passed your length validator and the
value that reached the database were different values. Truncation
differences between validation and execution are how smuggling bugs happen.

Endpoints whose parameters are all untyped produce an empty binding plan,
and the call collapses to the historic string path — the rollout changed
nothing for configs that don't opt in via typed validators. Pagination
reuses the same plan: the count query (`SELECT COUNT(*) FROM (…)`) executes
with the identical bindings, and the Arrow-streaming path binds the same way.

## The boundary, precisely

Here is the part this post exists for. Three kinds of template sites are
**not** bound, and fall back to the historical defense — typed validators
plus template discipline:

**Triple-brace sites (`{{{ params.x }}}`).** Raw interpolation is an
explicit operator request, and existing templates quote these sites
(`'{{{ params.name }}}'`). Mechanically rewriting `'{{{ x }}}'` to `'?'`
would produce a string literal containing a question mark — not a
placeholder — so the rewriter leaves triple-brace sites alone. Migrating
one is a manual, two-character edit: drop the surrounding quotes, drop a
brace.

**Untyped parameters.** If a field has no validator the classifier can map
to a SQL type, we don't guess. Guessing `VARCHAR` for everything would be
safe-ish but would silently change comparison semantics in existing
templates (`col = '42'` vs `col = 42`), and a security mechanism that
changes query results is a mechanism people turn off.

**Sites inside `{{#section}}` / `{{^section}}` blocks.** This one is a
correctness constraint, not a policy choice. Placeholder positions are
assigned by scanning the template *before* Mustache expands conditionals. A
`?` inside `{{#params.id}}…{{/params.id}}` would vanish from the rendered
SQL whenever `id` is absent, and every placeholder after it would shift —
the binding list and the statement would silently disagree about positions.
So the rewriter treats section interiors as opaque.

Why keep interpolation around at all? Because some template content is
structurally incapable of being a prepared-statement parameter. A parameter
can be a *value*; it cannot be an identifier, a table name, an `ORDER BY`
direction, or a conditionally-included clause. `FROM '{{{conn.path}}}'` and
`{{cache.table}}` change the shape of the statement, and no database's
prepare API accepts a placeholder there. Those sites are operator-controlled
rather than user-controlled — but the template language cannot enforce that
distinction, so we won't claim it does.

The defensible claim, then: **at bound sites, injection is structurally
impossible — the user value never enters SQL text. At unbound sites, flAPI
provides validator-guarded interpolation, same as before.** Whether your
endpoint gets the strong property everywhere user data flows is a property
of your template. Which brings us to:

## Writing templates so everything user-controlled is bound

Four rules, one verification step.

**1. Give every request field a typed validator.** This is what makes it
bindable — and `enum` counts, so constrained-vocabulary strings get bound
*and* whitelisted:

```yaml
request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
        max: 1000000
  - field-name: segment
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]
```

**2. Reference parameters with double braces, unquoted.** The `?` carries
its own type; quotes would demote it to a literal. (If you do leave quotes
around a bound site, the mistake fails loudly — binding a parameter into a
statement that has none errors at execution — rather than silently
interpolating.)

**3. Replace conditional sections around user values with NULL-tolerant
SQL.** Optional parameters bind `NULL` when absent, so the classic
optional-filter pattern works without Mustache conditionals:

```sql
SELECT c_custkey AS key, c_name AS name, c_acctbal AS balance
FROM '{{{conn.path}}}'
WHERE 1=1
  AND ({{ params.id }} IS NULL OR c_custkey = {{ params.id }})
  AND ({{ params.segment }} IS NULL OR c_mktsegment = {{ params.segment }})
```

renders (before binding) to:

```sql
SELECT c_custkey AS key, c_name AS name, c_acctbal AS balance
FROM './data/customers.parquet'
WHERE 1=1
  AND (? IS NULL OR c_custkey = ?)
  AND (? IS NULL OR c_mktsegment = ?)
```

with four bindings — `id` twice, `segment` twice, each converted once from
the same request value. Omit `id` and positions 1–2 bind `NULL`:
`(NULL IS NULL OR …)` short-circuits true and DuckDB's optimizer prunes the
branch. Keep `{{#sections}}` for genuinely structural variation (an
optional `ORDER BY`, a debug column) — just don't put user values inside
them and expect binding.

**4. Reserve `{{{ }}}` for operator-controlled structure.** Connection
properties, cache table names — things defined in YAML you deploy, not
values that arrive in a request.

**Verify with dry-run.** Any MCP client (or curl) can pass
`"_dryRun": true` in `tools/call` arguments; flAPI runs the full
validate → rewrite → render pipeline and returns the SQL it *would* execute,
without executing it:

```json
{
  "dry_run": true,
  "rendered_sql": "SELECT … WHERE 1=1 AND (? IS NULL OR c_custkey = ?) …",
  "params": {"id": "42"}
}
```

If a user-controlled value appears in `rendered_sql` as text instead of a
`?`, that site is interpolated, not bound. This is the audit we run on our
own configs.

## What this does not solve

- **It is not a general SQL firewall.** Cache-refresh templates render on
  the string path — their inputs are operator- and scheduler-supplied, not
  request parameters, but they are interpolated. Same for `conn.*` and
  `cache.*` expansion.
- **Validators still matter.** Unbound sites depend on them entirely, and
  even bound sites want range checks — `duckdb_bind_int64` will happily
  bind an integer your business logic considers absurd.
- **Binding does not authorize.** A perfectly-bound query can still read
  data the caller shouldn't see; that's what per-tool RBAC and column
  redaction are for, and they're a different post.
- **The strong property is per-site, not per-project.** We considered a
  strict mode that refuses to start when any `params.*` site is unbound;
  it doesn't exist yet. Until it does, the dry-run audit is manual.

flAPI is a single static C++17 binary with DuckDB 1.5.3 embedded
(`uvx --from flapi-io flapi` or `pip install flapi-io`), source-available
under BSL 1.1 — production use permitted, converts to MPL-2.0. The three
components described here are
[`src/sql_parameter_classifier.cpp`](https://github.com/DataZooDE/flapi/blob/main/src/sql_parameter_classifier.cpp),
[`src/prepared_template_rewriter.cpp`](https://github.com/DataZooDE/flapi/blob/main/src/prepared_template_rewriter.cpp),
and
[`src/query_executor.cpp`](https://github.com/DataZooDE/flapi/blob/main/src/query_executor.cpp)
— the classifier and rewriter total about 250 lines, and the binding path
in the executor another hundred. We'd genuinely welcome adversarial review
of all three.

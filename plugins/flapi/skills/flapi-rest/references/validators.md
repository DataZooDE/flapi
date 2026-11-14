# flAPI validator reference

Source of truth: `src/config_manager.cpp` (`parseEndpointValidators`, the keys
read) and `src/request_validator.cpp` (what is checked). Validators run on every
request before the template is rendered; any failure is HTTP 400
`{"errors":[{"field":"<name>","message":"<text>"}]}`. A field may list several
validators; all of them run.

## Types

| `type` | Aliases | Options read | Accepts | Error message |
|---|---|---|---|---|
| `int` | `integer` | `min`, `max` (inclusive; default INT_MIN / INT_MAX) | the WHOLE value is a 32-bit integer (`12`, `-3`; not `1.0`, `12abc`, `1; DROP`) | `Invalid integer value`, `Integer is less than the minimum allowed value`, `Integer is greater than the maximum allowed value` |
| `number` | `float`, `double` | none | one plain decimal: `-3`, `1.5`, `.5`, `1e3` (no spaces, hex, `NaN`) | `Value is not a valid number` |
| `boolean` | `bool` | none | `true`, `false`, `1`, `0`, any case | `Value is not a valid boolean (true, false, 1 or 0)` |
| `string` | - | `regex` (alias `pattern`; full match, ECMAScript), `min` / `max` (length; 0 = no limit), `min-length` / `max-length` (same, take precedence) | anything matching | `String is shorter than the minimum allowed length`, `String is longer than the maximum allowed length`, `Invalid string format` |
| `enum` | - | `allowedValues` (alias `values`; list, **required**, case-sensitive exact match) | one of the list | `Invalid enum value` |
| `email` | - | none | `local@domain.tld` (`[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}`) | `Invalid email format` |
| `uuid` | - | none | `8-4-4-4-12` hex, any version | `Invalid UUID format` |
| `date` | - | `min`, `max` (`YYYY-MM-DD`, inclusive) | `YYYY-MM-DD`, nothing after it | `Invalid date format`, `Date is before the minimum allowed date`, `Date is after the maximum allowed date` |
| `time` | - | `min`, `max` (`HH:MM:SS`, inclusive) | `HH:MM:SS`, 00-23 / 00-59 / 00-59 | `Invalid time format`, `Time is before the minimum allowed time`, `Time is after the maximum allowed time` |

Every type also reads `preventSqlInjection` (default `true`, see below). An
unknown `type` is accepted at load time and checks nothing.

Spellings: `regex`/`allowedValues` are canonical; `pattern`/`values` are
accepted aliases (when both are present the canonical key wins). An enum with
neither fails startup (`Missing required key: allowedValues`). `field-type: int`
on a field is read by nothing (load warning "`field-type` has no effect"); use
a validator `- type: int`.

## Other request-field checks

- `required: true` and the value is absent: `Required field is missing`.
- `request-fields-validation: true` on a GET endpoint, or the write default
  `operation.validate-before-write: true`: any parameter that is not a declared
  `field-name` (other than `limit` / `offset`) is `Unknown parameter not defined
  in endpoint configuration`.
- `limit` / `offset` on a paginated GET must be plain non-negative integers;
  otherwise a plain-text 400 `Invalid pagination parameters: ...`.
- Values for absent fields are not validated; a field with `default:` is
  validated with its default.

## Example: one of each

```yaml
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1
        max: 1000000
  - field-name: amount
    field-in: query
    validators:
      - type: number
  - field-name: active
    field-in: query
    validators:
      - type: boolean
  - field-name: code
    field-in: query
    validators:
      - type: string
        regex: '^[A-Z]{3}-[0-9]{4}$'
        min-length: 8
        max-length: 8
  - field-name: status
    field-in: query
    validators:
      - type: enum
        allowedValues: [active, inactive, pending]
  - field-name: email
    field-in: query
    validators:
      - type: email
  - field-name: order_ref
    field-in: query
    validators:
      - type: uuid
  - field-name: since
    field-in: query
    validators:
      - type: date
        min: "2020-01-01"
        max: "2030-12-31"
  - field-name: opens_at
    field-in: query
    validators:
      - type: time
        min: "06:00:00"
        max: "22:00:00"
```

## Typed validators decide binding

The first validator with a known type decides how `{{ params.x }}` (double
braces, outside any section) is bound as a prepared-statement parameter:

| Validator type | Bound as |
|---|---|
| `int`, `integer` | BIGINT |
| `number`, `float`, `double` | DOUBLE |
| `boolean`, `bool` | BOOLEAN |
| `date` | DATE |
| `time` | TIME |
| `string`, `enum`, `email`, `uuid` | VARCHAR |

A field with no validator is not bound: `{{ params.x }}` is then plain
HTML-escaped text substitution. An absent optional field binds NULL.

## The SQL-injection screen (`preventSqlInjection`)

Runs for every field unless ALL its validators set `preventSqlInjection: false`.
It rejects (`Potential SQL injection detected`) values containing the words
SELECT, INSERT, UPDATE, DELETE, DROP, TRUNCATE, ALTER, CREATE, TABLE, UNION,
EXEC, EXECUTE, SCRIPT, DECLARE, CAST, CONVERT (whole word, any case), the
substrings `';`, `--`, `/*`, `*/`, `xp_`, `sp_`, `1=1`, `1=2`, ` OR '1'='1`, or a
single quote together with an operator, parenthesis or SQL word anywhere in the
value. For `int`/`number`/`boolean`/`date`/`time` fields it only logs, because
the value is bound and type-checked.

It is a heuristic safety net with false positives (`O'Brien (draft)`,
`select a plan`). The defence is binding (`{{ params.x }}`) or an `enum` /
strict `regex` for anything interpolated with `{{{ }}}`. Set
`preventSqlInjection: false` only on fields the template uses through bound
`{{ }}` exclusively.

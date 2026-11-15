# YAML includes and environment variables in endpoint files

Includes are expanded as text before the YAML is parsed, so the file is not
valid YAML on its own. Paths are relative to the including file.

```yaml
# flapi: file group=includes file=common/shared.yaml
request:
  - field-name: nation
    field-in: query
    validators:
      - type: int
        min: 0
        max: 24
rate-limit:
  enabled: true
  max: 2
  interval: 60
```

```text
# flapi: endpoint group=includes file=by-nation.yaml
url-path: /by-nation
{{include:request from ../common/shared.yaml}}
{{include:rate-limit from ../common/shared.yaml}}
template-source: by-nation.sql
connection: [customers]
# flapi: check GET /by-nation?nation=5 -> 200
# flapi: check GET /by-nation?nation=6 -> 200
# flapi: check GET /by-nation?nation=7 -> 429
```

```sql
-- flapi: template group=includes file=by-nation.sql
SELECT count(*) AS customers FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.nation }} IS NULL OR c_nationkey = {{ params.nation }})
```

- `{{include from file.yaml}}` splices a whole file; `{{include:key from file.yaml}}`
  one top-level key; append `if env.VAR` / `if !env.VAR` / `if true` for a
  conditional include. Includes do not recurse.
- `{{env.VAR}}` works in any YAML value, only if `VAR` matches a regex in
  `template.environment-whitelist` (root config). An unlisted name stops startup;
  an empty whitelist allows none; `${VAR}` is left as literal text; a
  `{{env.X}}` inside an included file is not substituted. In SQL templates the
  same whitelist applies: `{{{ env.FLAPI_REGION }}}` (see `flapi-setup`).

# Reusing config and reading the environment

**Goal:** stop copying the same connection, auth or rate-limit block into every
endpoint, and keep secrets out of the file.

flAPI extends plain YAML with two features: environment-variable substitution and
includes.

---

## Environment variables
- Write environment variables as `{{env.VAR_NAME}}` anywhere in your YAML. (The
  `${VAR_NAME}` shell form is **not** supported and is left as literal text.)
- Only variables that match the whitelist in your root config may be read. The
  whitelist is a list of regular expressions, matched against the whole variable
  name (case-sensitively), and it lives under `template:` in the root config:
  ```yaml
  template:
    path: './sqls'
    environment-whitelist:
      - '^FLAPI_.*'     # allow all variables starting with FLAPI_
      - '^PROJECT_.*'   # optional additional prefixes
      - '^CONFIG_DIR$'  # or one exact variable
  ```
- **An empty or missing whitelist allows no variables.** A `{{env.NAME}}` that does
  not match stops flAPI at startup with an error naming every such variable and
  the key to add it to. It is never left in the file as literal text, because a
  literal `{{env.DB_PASSWORD}}` would silently become the password.
- The rule applies to the root config and to every endpoint file, and to `env.NAME`
  include conditions (below), and to every file pulled in with `{{include}}`: an
  included file gets the same substitution and the same whitelist check as the
  file that includes it (#165), so a shared `auth.yaml` can hold
  `password: '{{env.API_PASSWORD}}'`.
- `environment-whitelist` belongs under `template:`. A top-level
  `environment-whitelist:` is never read, so flAPI rejects it at startup.
- The whitelist must be in the root config file itself, not in a file it includes.
- A comment can mention a variable: `{{env.X}}` on a line that is only a comment is
  ignored. A comment after a value (`key: v # see {{env.X}}`) is still checked, and so is
  a `#` line inside a block scalar (`key: |`), which is text rather than a comment.

Examples:
```yaml
# Substitute inside strings
project-name: "{{env.PROJECT_NAME}}"

# Build include paths dynamically
template:
  path: "{{env.CONFIG_DIR}}/sqls"
```

## Include syntax
You can splice content from another YAML file directly into the current document.

- Basic include: `{{include from path/to/file.yaml}}`
- Section include: `{{include:top_level_key from path/to/file.yaml}}` includes only that key,
  **under its own name**
- Renamed section include: `{{include:top_level_key as other_key from path/to/file.yaml}}`
  includes that key's value under `other_key:`
- Conditional include: append `if <condition>` to any form

Conditions supported:
- `true` or `false`
- `env.VAR_NAME` (include if the variable exists and is non-empty)
- `!env.VAR_NAME` (include if the variable is missing or empty)

The variable in a condition must be whitelisted, like any other `{{env.NAME}}`.

Examples:
```yaml
# Include another YAML file relative to this file
{{include from common/settings.yaml}}

# Include only a section (top-level key) from a file
{{include:connections from shared/connections.yaml}}

# Conditional include based on an environment variable
{{include from overrides/dev.yaml if env.FLAPI_ENV}}

# Use env var in the include path
{{include from {{env.CONFIG_DIR}}/secrets.yaml}}
```

### One shared file, several variants: `as`

A section include keeps the section's own key. With a shared file that holds
variants side by side, include the one you want **as** the key the endpoint reads:

```yaml
# sqls/common.yaml (no url-path: a fragment, never loaded as an endpoint)
auth-prod:
  enabled: true
  type: basic
  users:
    - username: ops
      password: '{{env.FLAPI_OPS_PASSWORD}}'
auth-dev:
  enabled: false
```

```yaml
# sqls/orders.yaml
url-path: /orders
template-source: orders.sql
connection: [orders-db]
{{include:auth-prod as auth from common.yaml if env.FLAPI_PROD}}
{{include:auth-dev as auth from common.yaml if !env.FLAPI_PROD}}
```

Without `as`, `{{include:auth-prod from common.yaml}}` inserts an `auth-prod:` key.
Nothing reads it, so the endpoint has no `auth:` of its own: it falls back to the
top-level `auth:` of `flapi.yaml`, or is **public** when there is none. flAPI warns
about every endpoint key it does not read, at startup and in `--validate-config`,
and points at the fix:

```text
Unknown endpoint key `auth-prod` is ignored; nothing reads it: did you mean `auth`?
included sections keep their own name; use `{{include:auth-prod as auth from ...}}`
to insert it as `auth:`
```

It is a warning, not an error: `--validate-config` still passes, so check its output.

**The variants must exclude each other.** Exactly one `auth:` may end up in the
expanded file: give the two includes opposite conditions (`if env.X` and
`if !env.X`, as above), and do not also write an `auth:` of your own. When a key
occurs twice only the **first** is read, so `auth: {enabled: false}` above
`{{include:auth-prod as auth from common.yaml}}` leaves the endpoint public.
flAPI warns: ``SECURITY: endpoint key `auth` appears more than once ...``.

The directive is lower-case `as`; `AS` is a syntax error. Spaces inside the
braces are fine: `{{ include:auth-prod as auth from common.yaml }}`.

Resolution rules and behavior:
- Paths are resolved relative to the current file first; absolute paths are supported.
- Includes inside YAML comments are ignored (e.g., lines starting with `#`).
- Includes are expanded before the YAML is parsed.
- Includes do not recurse: include directives within included files are not processed further.
- Circular includes are guarded against within a single expansion pass; avoid cycles.

Tips:
- Prefer section includes (`{{include:...}}`) to avoid unintentionally overwriting unrelated keys.
- A section whose name is not an endpoint key (`auth-prod`, `cache-eu`) needs `as`.
- Keep shared blocks in small files (e.g., `connections.yaml`, `auth.yaml`) and include them where needed.

**Remote configuration.** When the config (or `template.path`) lives in a bucket or on http(s), an include may only
reach files **below its own remote directory**: absolute paths, `..`, and `s3://` URLs are refused ("Could not
resolve include path"), so `{{include from ../common/auth.yaml}}` works from a local checkout but not from the
bucket. Copy shared files into each release prefix (`releases/v42/common/`) and include them as `common/auth.yaml`.
See [cloud-storage.md](./cloud-storage.md#releasing-safely).

## How it works

Both features run **before** the YAML is parsed — the expanded text is what the
parser sees. That is why an include can appear anywhere a value can, and why a
malformed include shows up as a YAML syntax error.

- [spec/components/config-system.md](../spec/components/config-system.md) — loading and parsing order
- [CONFIG_REFERENCE](../CONFIG_REFERENCE.md) — every configuration key

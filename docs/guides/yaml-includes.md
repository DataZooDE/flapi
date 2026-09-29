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
  include conditions (below). **A `{{env.NAME}}` inside a file pulled in with
  `{{include}}` is not scanned** — it is left as literal text and is not checked
  (#165) — so keep references in the including file.
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
- Section include: `{{include:top_level_key from path/to/file.yaml}}` includes only that key
- Conditional include: append `if <condition>` to either form

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

Resolution rules and behavior:
- Paths are resolved relative to the current file first; absolute paths are supported.
- Includes inside YAML comments are ignored (e.g., lines starting with `#`).
- Includes are expanded before the YAML is parsed.
- Includes do not recurse: include directives within included files are not processed further.
- Circular includes are guarded against within a single expansion pass; avoid cycles.

Tips:
- Prefer section includes (`{{include:...}}`) to avoid unintentionally overwriting unrelated keys.
- Keep shared blocks in small files (e.g., `connections.yaml`, `auth.yaml`) and include them where needed.

## How it works

Both features run **before** the YAML is parsed — the expanded text is what the
parser sees. That is why an include can appear anywhere a value can, and why a
malformed include shows up as a YAML syntax error.

- [spec/components/config-system.md](../spec/components/config-system.md) — loading and parsing order
- [CONFIG_REFERENCE](../CONFIG_REFERENCE.md) — every configuration key

# Reusing config and reading the environment

**Goal:** stop copying the same connection, auth or rate-limit block into every
endpoint, and keep secrets out of the file.

flAPI extends plain YAML with two features: environment-variable substitution and
includes.

---

## Environment variables
- Write environment variables as `{{env.VAR_NAME}}` anywhere in your YAML.
- Only variables that match the whitelist in your root config are substituted:
  ```yaml
  template:
    path: './sqls'
    environment-whitelist:
      - '^FLAPI_.*'     # allow all variables starting with FLAPI_
      - '^PROJECT_.*'   # optional additional prefixes
  ```
- If the whitelist is empty or omitted, all environment variables are allowed.

Examples:
```yaml
# Substitute inside strings
project-name: "${{env.PROJECT_NAME}}"

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

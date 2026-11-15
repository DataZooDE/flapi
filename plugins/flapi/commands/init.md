---
description: Scaffold a new flAPI project from a description of your data source, validate it and start the server
argument-hint: "[project-dir] [what data you want to expose]"
---

Set up a new flAPI project for the user. Arguments: $ARGUMENTS

Use the `flapi-setup` skill for the details; use `flapi-rest` for the first endpoint.

1. Work out, asking only what you cannot infer:
   - project directory (default: current directory)
   - the data source: file (Parquet/CSV/JSON), database (Postgres, SQLite, MySQL,
     BigQuery, Snowflake, ...) or cloud storage (S3/GCS/Azure)
   - which credentials come from environment variables (never write secrets into YAML)
2. Check the tools: `flapi --version` (install with `pip install flapi-io` if missing,
   which also installs `flapii`).
3. Scaffold with `flapii project init <dir>` (or write `flapi.yaml` + `sqls/` by hand
   when the user wants a minimal layout). The sample endpoint reads
   `data/sample.csv`, which init creates, so it answers immediately. Then edit `flapi.yaml`:
   - one connection for the source, with `init:` SQL for the DuckDB extension it needs
   - every `{{env.VAR}}` it uses matched by `template.environment-whitelist`
4. Create one read endpoint over the real data (YAML + SQL template, validators on
   every parameter) so the user sees something working.
5. Validate: `flapi -c <dir>/flapi.yaml --validate-config` must report no errors.
6. Start it: `flapi -c <dir>/flapi.yaml --config-service` and call the endpoint with
   curl. Show the user the OpenAPI docs at `http://localhost:8080/doc`.
7. Finish with what was created and the next commands they will want
   (`/flapi:new-endpoint`, `/flapi:add-cache`, `/flapi:auth-setup`).

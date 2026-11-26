# Tavern patterns for flAPI

All stages below were run against flAPI v26.10.31 with `tavern>=3`. They assume
the `tests/common.yaml` from SKILL.md (`base_url` from `FLAPI_URL`) and the
endpoints from the `flapi-rest` write example (`/notes`) and the `flapi-auth`
Basic example (`/me`). Fenced as `text` because they are Tavern files, not
flAPI config.

## Response shapes to assert on

| Request | Status | Body |
|---|---|---|
| paginated GET (default) | 200 | `{"data":[...],"next":"/path?offset=..&limit=..&...","total_count":N}`; `next` is `""` on the last page |
| GET with `with-pagination: false` | 200 | bare JSON array `[...]` |
| validator failure | 400 | `{"errors":[{"field":"x","message":"..."}]}` (messages: flapi-rest `references/validators.md`) |
| bad `limit` / `offset` | 400 | JSON `{"errors":[{"field":"limit","message":"limit must be a non-negative integer"}]}` |
| POST write | 201 | `{"rows_affected":n,"data":[RETURNING rows]}` |
| PUT / PATCH write | 200 | same as POST |
| DELETE write | 200 | `{"rows_affected":n}` |
| SQL / constraint error | 500 | `{"error":"Internal Server Error","message":"...","request_id":"req-..."}`; no SQL, no paths (the error is in the server log under `request_id`) |
| missing / bad credentials | 401 | short or empty text body; `WWW-Authenticate: Basic realm="flAPI"` only when the header is missing |
| authenticated, none of `auth.roles` | 403 | `{"error":"Forbidden","message":"The authenticated principal does not hold any role this endpoint requires."}` |
| rate limited | 429 | `Rate limit exceeded. Try again later.` + `Retry-After` |
| cache not ready | 503 | `{"error":"cache_warming" / "cache_not_built" / "cache_build_failed",...}`, `Retry-After: 5` |

Response headers on reads: `X-Total-Count`, `X-Offset`, `X-Limit`, `X-Next`,
`X-Request-Id`.

## Write round trip with a saved value

```text
test_name: notes write round trip

includes:
  - !include common.yaml

stages:
  - name: create a note
    request:
      url: "{base_url}/notes"
      method: POST
      json:
        id: 42
        title: "O'Brien (draft)"
    response:
      status_code: 201
      json:
        rows_affected: 1
        data:
          - id: 42
            title: "O'Brien (draft)"
            done: false
      save:
        json:
          note_id: data[0].id

  - name: mark it done
    request:
      url: "{base_url}/notes/{note_id}"
      method: PATCH
      json:
        done: true
    response:
      status_code: 200
      json:
        data:
          - id: 42
            done: true
            title: !anystr
        rows_affected: 1

  - name: unknown body key is rejected
    request:
      url: "{base_url}/notes"
      method: POST
      json:
        id: 43
        title: x
        owner: me
    response:
      status_code: 400
      json:
        errors:
          - field: owner
            message: Unknown parameter not defined in endpoint configuration
```

Re-running against the same database file fails stage 1 with 500 (duplicate
key): delete the file or use fresh ids per run.

## Auth paths

```text
test_name: basic auth paths

includes:
  - !include common.yaml

stages:
  - name: no credentials
    request:
      url: "{base_url}/me"
    response:
      status_code: 401

  - name: valid credentials
    request:
      url: "{base_url}/me"
      auth:
        - reader
        - "{tavern.env_vars.READER_PASSWORD}"
    response:
      status_code: 200
      json:
        - username: reader
          roles: read
          auth_type: basic
```

A role check: a stage with a user who authenticates but holds none of the
endpoint's `auth.roles` expects `status_code: 403` and
`json: {error: Forbidden, message: !anystr}`. Under a global `auth:` block, an
endpoint with `auth: {enabled: false}` answers 200 without credentials: test
that too, so a later global change cannot silently close it.

Bearer / OIDC: send the header instead of `auth:`:

```text
    request:
      url: "{base_url}/orders"
      headers:
        Authorization: "Bearer {tavern.env_vars.TEST_TOKEN}"
```

## Matchers worth knowing

- `!anything`, `!anystr`, `!anyint`, `!anyfloat`, `!anybool`, `!anylist`,
  `!anydict` match by type.
- `status_code: [200, 400]` accepts either (use sparingly; it hides regressions).
- DOUBLE columns come back with full double precision
  (`711.55999999999994543`); match them with `!anyfloat` or round in SQL. DECIMAL
  columns are exact (`1.25`) for scale 0 up to 18 digits, or scale ≤ 6 with ≤ 9
  integer digits. Timestamps carry microseconds
  (`2024-01-01T10:00:00.123456Z`), NULL is `null`.
- Query `params:` values must be strings in Tavern (`limit: "2"`).

## Running

```bash
FLAPI_URL=http://127.0.0.1:8080 READER_PASSWORD=reader-pass \
  uv run --project tests pytest tests/test_notes.tavern.yaml -v
```

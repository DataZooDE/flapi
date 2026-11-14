# MCP write tools: a runnable CRUD example

Three write endpoints (POST, PUT, DELETE) that are also MCP tools, over a local
DuckDB file, with MCP basic auth and `allowed-roles` on every tool. Verified on
flAPI v26.10.31: each tool call answers 200 with the write's result, an
unauthenticated `tools/call` answers 401, and the REST routes answer 201/200/200.

```yaml
# flapi: config group=tasks
project-name: tasks-mcp
project-description: Task list with REST and MCP write tools
template:
  path: ./sqls
connections:
  tasks:
    init: |
      ATTACH IF NOT EXISTS './tasks.duckdb' AS tasksdb;
      CREATE TABLE IF NOT EXISTS tasksdb.tasks (id INTEGER PRIMARY KEY, title VARCHAR NOT NULL, done BOOLEAN DEFAULT false);
cors:
  allow-origins: [https://agent-console.example.com]
mcp:
  enabled: true
  auth:
    enabled: true
    type: basic
    users:
      - username: agent         # password: reader-pass
        password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
        roles: [editor]
```

```yaml
# flapi: endpoint group=tasks file=tasks-create.yaml
url-path: /tasks
method: POST
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
        min: 1
  - field-name: title
    field-in: body
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
operation:
  type: write
  returns-data: true
template-source: tasks-create.sql
connection: [tasks]
mcp-tool:
  name: create_task
  description: Creates a task with the given id and title and returns it. Writes data; fails if the id exists.
  allowed-roles: [editor]
# flapi: check POST /tasks body={"id":1,"title":"write docs"} -> 201
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"create_task","arguments":{"id":2,"title":"x"}}} -> 401
```

```sql
-- flapi: template group=tasks file=tasks-create.sql
INSERT INTO tasksdb.tasks (id, title) VALUES ({{ params.id }}, {{ params.title }})
RETURNING id, title, done
```

```yaml
# flapi: endpoint group=tasks file=tasks-update.yaml
url-path: /tasks/:id
method: PUT
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
  - field-name: done
    field-in: body
    required: true
    validators:
      - type: boolean
operation:
  type: write
  returns-data: true
template-source: tasks-update.sql
connection: [tasks]
mcp-tool:
  name: set_task_done
  description: Marks the task with this id done (true) or open (false) and returns it. Writes data.
  allowed-roles: [editor]
# flapi: check PUT /tasks/1 body={"done":true} -> 200
```

```sql
-- flapi: template group=tasks file=tasks-update.sql
UPDATE tasksdb.tasks SET done = {{ params.done }} WHERE id = {{ params.id }}
RETURNING id, title, done
```

```yaml
# flapi: endpoint group=tasks file=tasks-delete.yaml
url-path: /tasks/:id
method: DELETE
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
template-source: tasks-delete.sql
connection: [tasks]
mcp-tool:
  name: delete_task
  description: Deletes the task with this id; rows_affected is 0 when there was none. Writes data and cannot be undone.
  allowed-roles: [editor]
# flapi: check DELETE /tasks/1 -> 200
```

```sql
-- flapi: template group=tasks file=tasks-delete.sql
DELETE FROM tasksdb.tasks WHERE id = {{ params.id }}
```

```bash
MCP=http://localhost:8080/mcp/jsonrpc
curl -s -u agent:reader-pass $MCP -H 'Content-Type: application/json' -d \
  '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"create_task","arguments":{"id":3,"title":"ship"}}}'
# {"result":{"content":[{"type":"text","text":"{\"data\":[{\"id\":3,\"title\":\"ship\",\"done\":false}],\"rows_affected\":1}"}],
#  "structuredContent":{"rows":{"rows_affected":1,"data":[...]}}},...}
# set_task_done {"id":3,"done":true} -> same shape; delete_task {"id":3} -> {"rows_affected":1}, again -> 0
```


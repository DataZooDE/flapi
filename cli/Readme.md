## flapii – CLI for flAPI ConfigService

flapii is a modern Node.js CLI (TypeScript + Commander) to manage flAPI configuration over HTTP and MCP.

### Prerequisites
- Build the flAPI server first (C++):
  - `make release` (server is expected at `build/release/flapi`)
- Node.js 20+
- VS Code (for the extension)

### Install & Build (CLI)
```bash
# from repo root
make cli-build   # installs deps and builds the CLI
```

Or manually:
```bash
cd cli
npm ci
npm run build
```

Optional: add an npm script or install globally to use `flapii` directly (package.json exposes a bin).

Run via npx (no global install):
```bash
# from repo root
npx ./cli --help
npx ./cli ping

# from inside the cli directory
cd cli && npx . --help
cd cli && npx . ping

# ensure a fresh build first
npm --prefix cli run build && npx ./cli ping
```

Alternative methods:
```bash
# run the compiled file directly
node cli/dist/index.js --help

# or use npm exec
npm exec --prefix cli flapii -- --help
```

### Configuration
flapii reads configuration from (highest precedence first):
1) CLI flags (e.g., `--base-url`, `--config`)
2) Environment variables (`FLAPI_BASE_URL`, `FLAPI_CONFIG`, `FLAPI_TOKEN`, ...)
3) YAML file (`flapi.yaml`) discovered in CWD/parents or `~/.config/flapii/flapi.yaml`.

Common env vars:
- `FLAPI_BASE_URL` (default: `http://localhost:8080`)
- `FLAPI_CONFIG` (path to `flapi.yaml`)
- `FLAPI_TOKEN` (bearer token if your server requires auth)
- `FLAPI_CONFIG_SERVICE_TOKEN` (config service authentication token)

Global options (partial):
```
--config <path>         Path to flapi.yaml
--base-url <url>        Base URL of the server (default http://localhost:8080)
--auth-token <token>    Bearer token
--timeout <seconds>     Request timeout (default 10)
--retries <count>       Retries for failed requests (default 2)
--insecure              Disable TLS verification
--output <json|table>   Output format (default json)
--json-style <camel|hyphen> JSON key casing for display
```

### Quick Start
Start the server with a config:
```bash
./build/release/flapi --port 8080 --config examples/flapi.yaml
```

Ping the server:
```bash
node dist/index.js ping --base-url http://localhost:8080
```

Show effective CLI configuration:
```bash
node dist/index.js config show --base-url http://localhost:8080 --output table
```

### Config Commands

#### Log Level Management
Control the backend's runtime log level without restarting the server:

```bash
# List valid log levels
node dist/index.js config log-level list

# Get current log level
node dist/index.js config log-level get \
  --base-url http://localhost:8080 \
  --config-service-token YOUR_TOKEN

# Set log level (debug, info, warning, error)
node dist/index.js config log-level set debug \
  --base-url http://localhost:8080 \
  --config-service-token YOUR_TOKEN

# Using environment variables
export FLAPI_BASE_URL=http://localhost:8080
export FLAPI_CONFIG_SERVICE_TOKEN=YOUR_TOKEN
node dist/index.js config log-level set info
```

**Note:** Log level changes take effect immediately but reset on server restart.

#### Validate Configuration
```bash
node dist/index.js config validate \
  --base-url http://localhost:8080 \
  --config examples/flapi.yaml
```

### Project
```bash
# Get project config (JSON)
node dist/index.js project get --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### Endpoints
List endpoints:
```bash
node dist/index.js endpoints list --output table \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Get one endpoint:
```bash
node dist/index.js endpoints get /customers/ --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Create/update/delete with payload files (JSON or YAML):
```bash
# Create
node dist/index.js endpoints create \
  --file cli/test/fixtures/endpoint.json \
  --base-url http://localhost:8080 --config examples/flapi.yaml

# Update
node dist/index.js endpoints update /integration-test \
  --file cli/test/fixtures/endpoint.json \
  --base-url http://localhost:8080 --config examples/flapi.yaml

# Delete
node dist/index.js endpoints delete /integration-test --force \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### MCP
List tools/resources/prompts:
```bash
node dist/index.js mcp tools list --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml

node dist/index.js mcp resources list --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml

node dist/index.js mcp prompts list --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Get a specific MCP entity:
```bash
node dist/index.js mcp tools get <name> --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### Templates
List all templates:
```bash
node dist/index.js templates list --output table \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Get a specific template:
```bash
node dist/index.js templates get /customers/ --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Update a template:
```bash
# From a file
node dist/index.js templates update /customers/ \
  --file ./template.sql \
  --base-url http://localhost:8080 --config examples/flapi.yaml

# From stdin
echo "SELECT * FROM customers;" | node dist/index.js templates update /customers/ --stdin \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Expand template with parameters:
```bash
node dist/index.js templates expand /customers/ \
  --params '{"id": 123}' --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Test template syntax:
```bash
node dist/index.js templates test /customers/ \
  --params '{"id": 123}' \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### Cache
List all cache configurations:
```bash
node dist/index.js cache list --output table \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Get cache configuration for an endpoint:
```bash
node dist/index.js cache get /customers/ --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Update cache configuration:
```bash
node dist/index.js cache update /customers/ \
  --enabled true --ttl 300 --max-size 1000 \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Refresh endpoint cache:
```bash
node dist/index.js cache refresh /customers/ --force \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### Schema
Get database schema information:
```bash
node dist/index.js schema get --output json \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

Refresh schema information:
```bash
node dist/index.js schema refresh --force \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

List database connections:
```bash
node dist/index.js schema connections --output table \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

List tables in connections:
```bash
node dist/index.js schema tables --connection bigquery-lakehouse \
  --base-url http://localhost:8080 --config examples/flapi.yaml
```

### Output & Styling
- `--output json` prints machine-readable JSON (useful for scripting)
- `--output table` prints styled tables and panels for humans
- `--json-style` controls key casing for display (`camel` or `hyphen`)

### Troubleshooting
- Server not reachable: verify `--base-url` or `FLAPI_BASE_URL`
- Config not found: pass `--config` or set `FLAPI_CONFIG`
- JSON parsing in scripts: always add `--output json`

### Tests (CLI)
```bash
# From repo root
make cli-test

# Or manually
cd cli
npm test

# Integration tests (start a real server automatically)
npm run test:integration
```

---

## VS Code Extension – flapi Development Tools

A VS Code extension that lets you browse endpoints, validate configs, and interact with a running flapi instance using the files in your workspace.

### Build the Extension
```bash
# From repo root
make vscode-build
```
This builds the shared library and the VS Code extension bundle.

### Run the Extension (fastest path)
```bash
# 1) Ensure your flapi server is running
./build/release/flapi --config examples/flapi.yaml --port 8080

# 2) Launch VS Code with the extension loaded from the workspace
code --extensionDevelopmentPath ./cli/vscode-extension
```
This opens a new VS Code window with the flapi extension loaded from `cli/vscode-extension`.

### Configure the Extension
In VS Code:
- Open Settings and search for "flapi"
  - `flapi.serverUrl` → default `http://localhost:8080`
  - `flapi.configFile` → default `flapi.yaml`
- Alternatively, in settings.json:
```json
{
  "flapi.serverUrl": "http://localhost:8080",
  "flapi.configFile": "flapi.yaml"
}
```

### What You’ll See
- A "flapi" icon in the Activity Bar
- "Endpoints" tree view listing all configured endpoints
- Context commands (via the command palette or right-click):
  - `flapi: Refresh Endpoints`
  - `flapi: Test Template`
  - `flapi: Expand Template`
  - `flapi: Validate Configuration`

### Live Development (watch build)
For active development with auto-rebuilds:
```bash
# Terminal 1 – watch shared + extension builds
make vscode-dev

# Terminal 2 – start VS Code with the dev extension
code --extensionDevelopmentPath ./cli/vscode-extension
```
When you edit code under `cli/vscode-extension/src` or `cli/shared/src`, rebuilds occur automatically. Reload the Extension Host window to pick up changes.

### Tips
- The extension activates automatically if your workspace contains a `flapi.yaml`.
- Keep the server running locally while you edit and test templates or endpoints.
- You can still use the CLI and the extension side-by-side — both talk to the same server and reuse the same client code under `cli/shared`.



# Project Initialization Guide

This guide covers the `flapii project init` command, which scaffolds a new flapi project with directories, configuration files, and examples.

## Quick Start

### Initialize in Current Directory

```bash
flapii project init

Setting up flapi project in current directory...
✅ Created directories
✅ Created flapi.yaml
✅ Created sample endpoint
✅ Created .gitignore
✓ Configuration is valid

✨ Project Setup Complete!
```

### Create New Project Directory

```bash
flapii project init my-api-project

Setting up flapi project: my-api-project
✅ Created directories
✅ Created flapi.yaml
✅ Created sample endpoint
✅ Created .gitignore
✓ Configuration is valid

✨ Project Setup Complete!
```

## Directory Structure

The init command creates this structure:

```
[PROJECT_NAME]/
├── flapi.yaml           # Main configuration file
├── sqls/                # Endpoint definitions and SQL templates
│   ├── sample.yaml      # Example endpoint configuration
│   └── sample.sql       # Example SQL template
├── data/                # Data files (Parquet, CSV, SQLite, etc.)
├── common/              # Reusable configuration templates
│   ├── auth.yaml        # Authentication configuration template
│   └── rate-limit.yaml  # Rate limiting configuration template
└── .gitignore           # Git ignore rules for flapi projects
```

### Directory Purposes

**`sqls/`** - Endpoint definitions and SQL templates
- Place your endpoint YAML files here
- Each endpoint should have a corresponding SQL template
- Example: `sqls/customers.yaml` and `sqls/customers.sql`

**`data/`** - Data files
- Store your data files here (Parquet, CSV, SQLite, etc.)
- Reference from `flapi.yaml` connections
- Git-ignored by default (add your own data)

**`common/`** - Shared configuration templates
- Reusable authentication configurations
- Shared rate-limiting rules
- Include these in endpoint configs using Mustache

**`.gitignore`** - Git ignore rules
- Prevents committing data files, cache, and sensitive configs
- Ensures secrets aren't accidentally committed

## File Descriptions

### flapi.yaml - Main Configuration

The project's main configuration file:

```yaml
project-name: my-flapi-project
description: Example flapi configuration

connections:
  sample-data:
    properties:
      path: './data/sample.parquet'

template-source: sqls
```

**Key Fields:**
- `project-name` - Unique identifier for your project
- `description` - Human-readable project description
- `connections` - Data source configurations
- `template-source` - Directory containing endpoint definitions

**Edit to:**
1. Add your database connections (Postgres, BigQuery, etc.)
2. Configure DuckDB settings (memory, threads)
3. Set authentication and rate limiting
4. Enable caching strategies

### sqls/sample.yaml - Example Endpoint

Demonstrates endpoint configuration structure:

```yaml
url-path: /sample
request:
  - field-name: id
    field-in: query
    description: Sample ID to filter
    required: false
    validators:
      - type: int
        min: 1

template-source: sample.sql
connection: [sample-data]
```

**Key Concepts:**
- **url-path** - REST endpoint path
- **request** - Parameters accepted by endpoint
- **field-name** - Parameter name (e.g., `id`)
- **field-in** - Where parameter comes from (`query`, `path`, `body`)
- **validators** - Input validation rules
- **template-source** - SQL file to execute
- **connection** - Which database to query

**Customize by:**
1. Changing the URL path
2. Adding more parameters
3. Pointing to your SQL template
4. Referencing your connection

### sqls/sample.sql - Example SQL Template

Demonstrates Mustache template syntax:

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
{{#params.id}}
WHERE id = {{ params.id }}
{{/params.id}}
LIMIT 100
```

**Template Features:**
- **Triple braces** `{{{ }}}` - String parameter (auto-escaped)
- **Double braces** `{{ }}` - Numeric parameter (no escaping)
- **Conditional sections** `{{#params.id}}...{{/params.id}}` - Optional parts
- **Connection properties** `conn.path` - Access connection config

**Security Pattern:**
Always use triple braces for user-supplied strings to prevent SQL injection:
```sql
-- ✓ CORRECT - string parameter with triple braces
WHERE name = '{{{ params.name }}}'

-- ✗ WRONG - string parameter without escaping
WHERE name = '{{ params.name }}'

-- ✓ CORRECT - numeric parameter with double braces
WHERE id = {{ params.id }}
```

### common/auth.yaml - Authentication Template

Reusable authentication configuration:

```yaml
# Reusable authentication configuration
# Reference in endpoints with: {{include from common/auth.yaml}}

auth:
  type: basic
  users:
    - username: admin
      password: ${API_PASSWORD}

# Uncomment for JWT
# auth:
#   type: jwt
#   token-key: ${JWT_SECRET}
#   issuer: "example.com"
```

**Usage:**
1. Choose authentication type (basic, jwt, etc.)
2. Set credentials from environment variables
3. Reference in endpoint configs

### common/rate-limit.yaml - Rate Limiting Template

Reusable rate limiting configuration:

```yaml
rate-limit:
  enabled: true
  requests: 100
  window: 60  # seconds
```

**Customize:**
- `requests` - Number of allowed requests
- `window` - Time window in seconds (100 requests per 60 seconds = ~1.7 req/sec)

### .gitignore - Git Ignore Rules

Prevents accidental commits of:
- Data files (`data/`)
- Cache files (`*.db`, `*.ducklake`)
- Environment variables (`.env`)
- IDE settings (`.vscode/`, `.idea/`)
- Build artifacts

## Command Options

### Skip Validation

```bash
flapii project init my-api --skip-validation
```

Skip configuration validation after setup. Useful for CI/CD or when you know there are issues.

### Only Core Files

```bash
flapii project init my-api --no-examples
```

Create only:
- `flapi.yaml` - Configuration
- Directory structure
- `.gitignore`

Skips example endpoint and SQL files.

### Force Overwrite

```bash
flapii project init my-api --force
```

Overwrite existing files without prompting. Useful for:
- Resetting a project to defaults
- Updating example files
- CI/CD automation

Without `--force`, existing files are skipped with a warning.

### Advanced Mode

```bash
flapii project init my-api --advanced
```

Include additional templates and examples for advanced use cases (Phase 3 feature).

## Common Workflows

### 1. Initialize a New Project from Scratch

```bash
# Create project directory
flapii project init my-analytics-api

# Edit configuration
nano my-analytics-api/flapi.yaml

# Add your database connection
# Edit connection properties to match your database

# Create your first endpoint
nano my-analytics-api/sqls/customers.yaml
nano my-analytics-api/sqls/customers.sql

# Validate configuration
flapii config validate --config my-analytics-api/flapi.yaml

# Start server
./flapi -c my-analytics-api/flapi.yaml
```

### 2. Add flapi to Existing Project

```bash
# Navigate to your project
cd my-existing-project

# Initialize in current directory
flapii project init

# This creates sqls/, data/, common/ subdirectories
# And flapi.yaml configuration file

# Copy existing data to data/ directory
cp /path/to/my/data.parquet ./data/

# Update flapi.yaml to reference your data
nano flapi.yaml

# Create endpoints
mkdir -p sqls
nano sqls/my_endpoint.yaml
nano sqls/my_endpoint.sql
```

### 3. Reset Project to Defaults

```bash
# If you've made mistakes or want fresh examples
flapii project init my-api --force

# This overwrites all template files with defaults
# Your data/ and custom configs won't be affected unless they match template names
```

### 4. Setup for Team Collaboration

```bash
# Initialize with version-controlled structure
flapii project init team-api

# Share configuration across team
git add team-api/flapi.yaml team-api/sqls/ team-api/common/
git add team-api/.gitignore

# Each team member can:
git clone <repo>
# And add their own local data/
cp /path/to/my/data.parquet team-api/data/
```

## Configuration Examples

### Example 1: Parquet File Connection

```yaml
# flapi.yaml
connections:
  sales-data:
    properties:
      path: './data/sales.parquet'

template-source: sqls
```

### Example 2: Postgres Database

```yaml
# flapi.yaml
connections:
  postgres-db:
    properties:
      host: $DB_HOST
      port: $DB_PORT
      user: $DB_USER
      password: $DB_PASSWORD
      database: $DB_NAME

template-source: sqls
```

**Note:** Reference environment variables with `$VARIABLE_NAME` syntax

### Example 3: BigQuery Connection

```yaml
# flapi.yaml
connections:
  bigquery-lakehouse:
    init: |
      FORCE INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
      LOAD 'bigquery';
    properties:
      project_id: 'my-gcp-project'
      dataset: 'my_dataset'

template-source: sqls
```

### Example 4: Multiple Connections

```yaml
# flapi.yaml
connections:
  postgres-main:
    properties:
      host: localhost
      port: 5432
      user: $DB_USER
      password: $DB_PASSWORD

  analytics-warehouse:
    properties:
      path: './data/warehouse.parquet'

  external-api:
    properties:
      url: 'https://api.example.com'

template-source: sqls
```

## Troubleshooting

### "Project directory already exists"

The directory exists but flapi wasn't initialized there:

```bash
# Option 1: Initialize in the existing directory
cd my-existing-dir
flapii project init

# Option 2: Use --force to overwrite
flapii project init existing-dir --force
```

### "Permission denied creating directory"

You don't have write permissions:

```bash
# Check permissions
ls -la my-api

# Fix permissions (if it's your directory)
chmod u+w my-api

# Or create in a directory where you have permissions
cd ~/projects  # Or your user directory
flapii project init my-api
```

### "Configuration validation failed"

Generated config has issues:

```bash
# Check what went wrong
flapii config validate --config my-api/flapi.yaml

# Review generated files
cat my-api/flapi.yaml
cat my-api/sqls/sample.yaml

# Manual fixes usually involve:
# - Correct connection names
# - Verify path syntax
# - Check template references
```

### "flapi command not found when trying to validate"

The flapi server binary isn't in your PATH:

```bash
# Option 1: Build flapi
make release
export PATH="$PWD/build/release:$PATH"

# Option 2: Skip validation
flapii project init my-api --skip-validation

# Option 3: Validate later with full path
./build/release/flapi -c my-api/flapi.yaml
```

## Best Practices

### 1. Use Environment Variables for Secrets

```yaml
# ✓ Good - credentials from environment
connections:
  database:
    properties:
      password: ${DB_PASSWORD}

# ✗ Bad - hardcoded credentials
connections:
  database:
    properties:
      password: "my-secret-password"
```

### 2. Use Template Variables with Proper Escaping

```sql
-- ✓ Good - triple braces for strings
WHERE name = '{{{ params.customer_name }}}'

-- ✓ Good - double braces for numbers
WHERE id = {{ params.customer_id }}

-- ✗ Bad - missing escaping
WHERE name = '{{ params.customer_name }}'
```

### 3. Use Conditional Sections for Optional Parameters

```sql
SELECT * FROM customers
{{#params.country}}
WHERE country = '{{{ params.country }}}'
{{/params.country}}
LIMIT {{ params.limit | 100 }}
```

### 4. Add Comments to Shared Templates

```yaml
# common/auth.yaml

# This authentication config is used by all admin endpoints
# Update the password in .env and reference with ${ADMIN_PASSWORD}

auth:
  type: basic
  users:
    - username: admin
      password: ${ADMIN_PASSWORD}
```

### 5. Keep Project Configuration in Version Control

```bash
# Track these
git add flapi.yaml
git add sqls/
git add common/
git add .gitignore

# Don't track these
git add .gitignore  # .env, data/, *.db already ignored
# git add data/      # Skip data files
# git add .env       # Skip secrets
```

## Advanced Setup (Phase 3)

### AI-Assisted Project Configuration

Coming soon: Initialize projects with AI assistance:

```bash
flapii project init my-api --ai

# Describe your project in natural language:
# "I need to serve BigQuery analytics data with customer and order tables"

# AI generates:
# - Appropriate connection configuration
# - Relevant example endpoints
# - Optimized DuckDB settings
# - Caching strategy recommendations
```

## Next Steps After Initialization

1. **Edit Configuration**
   ```bash
   nano flapi.yaml
   # Add your database connections
   # Configure DuckDB settings
   ```

2. **Create Endpoints**
   ```bash
   nano sqls/my_endpoint.yaml
   nano sqls/my_endpoint.sql
   ```

3. **Validate Configuration**
   ```bash
   flapii config validate --config flapi.yaml
   ```

4. **Start Development Server**
   ```bash
   ./flapi -c flapi.yaml
   ```

5. **Test Endpoints**
   ```bash
   curl http://localhost:8080/my-endpoint?param=value
   ```

## Getting Help

- **Configuration syntax**: See `docs/config.md`
- **SQL templates**: See `docs/TEMPLATE_LOOKUP_FEATURE.md`
- **Endpoint examples**: See `examples/` directory
- **CLI commands**: Run `flapii --help`
- **Specific command**: Run `flapii project init --help`


/**
 * Embedded file templates for project initialization
 * These templates are used when creating a new flapi project
 */

export const PROJECT_TEMPLATES = {
  /**
   * Main flapi configuration file
   * Contains project metadata, connections, and settings
   */
  'flapi.yaml': `project-name: my-flapi-project
description: Example flapi configuration

connections:
  sample-data:
    properties:
      path: './data/sample.parquet'

template-source: sqls
`,

  /**
   * Example endpoint configuration
   * Demonstrates basic endpoint structure with parameters and validators
   */
  'sqls/sample.yaml': `url-path: /sample
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
`,

  /**
   * Example SQL template
   * Shows Mustache template syntax with connection properties and parameters
   * Demonstrates:
   * - Triple braces {{{ }}} for string escaping
   * - Double braces {{ }} for numeric values
   * - Conditional sections {{#params.id}}...{{/params.id}}
   * - Connection property access {{{ conn.path }}}
   */
  'sqls/sample.sql': `SELECT * FROM read_parquet('{{{ conn.path }}}')
{{#params.id}}
WHERE id = {{ params.id }}
{{/params.id}}
LIMIT 100
`,

  /**
   * Git ignore rules for flapi projects
   * Prevents committing sensitive files, cache files, and data
   */
  '.gitignore': `# flapi data and cache
data/
*.db
*.ducklake
cache/

# Build artifacts
dist/
build/

# IDE
.vscode/
.idea/
*.swp
*.swo

# System
.DS_Store
*.tmp

# Environment variables
.env
.env.local

# Logs
*.log
`,

  /**
   * Reusable authentication configuration template
   * Can be included in endpoint configs using {{include from common/auth.yaml}}
   * Provides examples of Basic Auth and JWT
   */
  'common/auth.yaml': `# Reusable authentication configuration
# Reference in endpoints with: {{include from common/auth.yaml}}

# Example Basic Auth
auth:
  type: basic
  users:
    - username: admin
      password: \${API_PASSWORD}

# Example JWT (uncomment to use)
# auth:
#   type: jwt
#   token-key: \${JWT_SECRET}
#   issuer: "example.com"
`,

  /**
   * Reusable rate-limit configuration template
   * Can be included in endpoint configs for consistent rate limiting
   */
  'common/rate-limit.yaml': `# Reusable rate limiting configuration
# Reference in endpoints with: {{include from common/rate-limit.yaml}}

# Example rate limiting: 100 requests per minute
rate-limit:
  enabled: true
  requests: 100
  window: 60  # seconds
`,
};

/**
 * Get all template file paths (relative to project directory)
 */
export function getTemplateFilePaths(): string[] {
  return Object.keys(PROJECT_TEMPLATES);
}

/**
 * Get template content by file path
 */
export function getTemplateContent(filePath: string): string | undefined {
  return PROJECT_TEMPLATES[filePath as keyof typeof PROJECT_TEMPLATES];
}

/**
 * Check if a path is a directory template (needs to be created)
 */
export function isDirectoryTemplate(filePath: string): boolean {
  // Directories that should always be created
  const directoryPaths = ['sqls/', 'data/', 'common/'];
  return directoryPaths.some((dir) => filePath.includes(dir) && filePath.endsWith('/'));
}

/**
 * Get directories that should be created
 */
export function getDirectoriesToCreate(): string[] {
  return ['sqls', 'data', 'common'];
}

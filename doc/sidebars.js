/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  tutorialSidebar: [
    {
      type: 'category',
      label: 'Getting Started',
      items: [
        'getting-started/introduction',
        'getting-started/installation',
        'getting-started/first-api',
        'getting-started/configuration',
        'getting-started/features',
        'getting-started/contributing',
      ],
    },
    {
      type: 'category',
      label: 'Connections',
      items: [
        'connections/overview',
        'connections/duckdb-config',
        'connections/bigquery',
        'connections/postgresql',
        'connections/sap',
        'connections/parquet',
        'connections/custom',
      ],
    },
    {
      type: 'category',
      label: 'Endpoints',
      items: [
        'endpoints/overview',
        'endpoints/sql-templates',
        'endpoints/parameters',
        'endpoints/validation',
        'endpoints/response-format',
        {
          type: 'category',
          label: 'Caching',
          items: [
            'endpoints/caching/overview',
            'endpoints/caching/strategies',
            'endpoints/caching/invalidation',
          ],
        },
        {
          type: 'category',
          label: 'Authentication',
          items: [
            'endpoints/auth/overview',
            'endpoints/auth/basic-auth',
            'endpoints/auth/jwt',
            'endpoints/auth/custom',
          ],
        },
        'endpoints/rate-limiting',
        'endpoints/error-handling',
      ],
    },
  ],
};

module.exports = sidebars; 
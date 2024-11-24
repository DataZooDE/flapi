/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docs: [
    {
      type: 'category',
      label: '🚀 Start Here',
      className: 'sidebar-category-top',
      collapsed: false,
      collapsible: false,
      items: [
        'getting-started/quickstart',
        'getting-started/build-from-source',
        'getting-started/configuration',
        'getting-started/deployment',
        'getting-started/contributing',
      ],
    },
    {
      type: 'category',
      label: '🔌 Connections',
      className: 'sidebar-category-top',
      collapsed: false,
      collapsible: false,
      items: [
        'connections/overview',
        'connections/examples',
      ],
    },
    {
      type: 'category',
      label: '🛣️ Endpoints',
      className: 'sidebar-category-top',
      collapsed: false,
      collapsible: false,
      items: [
        'endpoints/overview',
        'endpoints/sql-templates',
        'endpoints/parameters',
        'endpoints/validation',
        'endpoints/response-format',
        'endpoints/error-handling',
        'endpoints/rate-limiting',
        'endpoints/authentication',
        'endpoints/caching',
      ],
    },
  ],
};

module.exports = sidebars; 
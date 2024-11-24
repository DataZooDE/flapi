const config = {
  title: 'flAPI | Make your data fly!',
  tagline: 'Instant SQL based APIs powered by DuckDB',
  favicon: 'img/favicon.ico',
  url: 'https://your-domain.com',
  baseUrl: '/',
  organizationName: 'datazoode',
  projectName: 'flapi',
  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'warn',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.js',
          editUrl: 'https://github.com/datazoode/flapi/tree/main/doc/',
        },
        blog: false,
        theme: {
          customCss: [
            './src/css/custom.css',
            './src/css/documentation.css',
          ],
        },
      },
    ],
  ],

  themeConfig: {
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    image: 'img/flapi-social-card.png',
    navbar: {
      title: 'flAPI',
      logo: {
        alt: 'flAPI Logo',
        src: 'img/logo.svg',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docs',
          position: 'left',
          label: 'Documentation',
        },
        {
          href: 'https://github.com/datazoode/flapi',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {
              label: 'Start',
              to: '/docs/getting-started/introduction',
            },
            {
              label: 'API Reference',
              to: '/docs/api/overview',
            },
          ],
        },
        {
          title: 'Community',
          items: [
            {
              label: 'GitHub Discussions',
              href: 'https://github.com/datazoode/flapi/discussions',
            },
            {
              label: 'Issues',
              href: 'https://github.com/datazoode/flapi/issues',
            },
          ],
        },
        {
          title: 'More',
          items: [
            {
              label: 'GitHub',
              href: 'https://github.com/datazoode/flapi',
            },
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} flAPI. Built with Docusaurus.`,
    },
    prism: {
      theme: require('prism-react-renderer').themes.github,
    },
    docs: {
      sidebar: {
        hideable: false,
        autoCollapseCategories: false,
      },
    },
  },
};

module.exports = config; 
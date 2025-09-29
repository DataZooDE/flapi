import type * as monaco from 'monaco-editor';

export function registerMustacheLanguage(m: typeof monaco) {
  m.languages.register({ id: 'mustache-sql' });

  m.languages.setMonarchTokensProvider('mustache-sql', {
    defaultToken: '',
    tokenPostfix: '.sql',
    tokenizer: {
      root: [
        [/{{![^}]*}}/, 'comment'],
        [/{{[{#^/]?/, { token: 'delimiter.mustache', next: '@mustache', nextEmbedded: 'sql' }],
        [/.*/, ''],
      ],
      mustache: [
        [/}}/, { token: 'delimiter.mustache', next: '@pop', nextEmbedded: '@pop' }],
        [/[^}]+/, 'metatag'],
      ],
    },
  });
}

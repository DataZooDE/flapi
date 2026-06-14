import { describe, it, expect } from 'vitest';
import {
  parseIncludeDirectives,
  resolveExtendedYaml,
} from '../src/codelens/extendedYaml';

const COMMON = `
request:
  - field-name: id
    field-in: query
auth:
  enabled: true
connection:
  - northwind
template-source: customers.sql
`;

describe('parseIncludeDirectives', () => {
  it('parses a basic include', () => {
    const dirs = parseIncludeDirectives('{{include:request from customer-common.yaml}}');
    expect(dirs).toEqual([
      { section: 'request', file: 'customer-common.yaml', condition: undefined, raw: '{{include:request from customer-common.yaml}}' },
    ]);
  });

  it('captures the conditional clause', () => {
    const dirs = parseIncludeDirectives('{{include:auth from common.yaml if env.SECURE}}');
    expect(dirs).toHaveLength(1);
    expect(dirs[0].section).toBe('auth');
    expect(dirs[0].condition).toBe('env.SECURE');
  });

  it('ignores commented-out directives', () => {
    expect(parseIncludeDirectives('# {{include:auth from common.yaml}}')).toEqual([]);
  });
});

describe('resolveExtendedYaml', () => {
  const readFile = (file: string) =>
    file === 'customer-common.yaml' ? COMMON : undefined;

  it('inlines an included section', () => {
    const text = `
url-path: /customers
method: GET
{{include:request from customer-common.yaml}}
{{include:connection from customer-common.yaml}}
`;
    const result = resolveExtendedYaml(text, readFile);
    expect(result['url-path']).toBe('/customers');
    expect(result.method).toBe('GET');
    expect(result.request).toEqual([{ 'field-name': 'id', 'field-in': 'query' }]);
    expect(result.connection).toEqual(['northwind']);
    // auth was not requested, so it must not leak in
    expect(result.auth).toBeUndefined();
  });

  it('resolves conditional includes unconditionally', () => {
    const text = `url-path: /x\n{{include:auth from customer-common.yaml if env.SECURE}}`;
    const result = resolveExtendedYaml(text, readFile);
    expect(result.auth).toEqual({ enabled: true });
  });

  it('keeps an explicit host value over the include', () => {
    const text = `template-source: override.sql\n{{include:template-source from customer-common.yaml}}`;
    const result = resolveExtendedYaml(text, readFile);
    expect(result['template-source']).toBe('override.sql');
  });

  it('falls back gracefully when the referenced file is missing', () => {
    const text = `url-path: /x\n{{include:request from missing.yaml}}`;
    const result = resolveExtendedYaml(text, () => undefined);
    expect(result['url-path']).toBe('/x');
    expect(result.request).toBeUndefined();
  });
});

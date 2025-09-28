import { describe, it, expect } from 'vitest';
import { applyOutputOverride } from '../../src/lib/options';

const config = {
  baseUrl: 'http://localhost:8080',
  timeout: 10,
  retries: 2,
  verifyTls: true,
  output: 'json' as const,
  jsonStyle: 'camel' as const,
  debugHttp: false,
  quiet: false,
  yes: false,
};

describe('applyOutputOverride', () => {
  it('returns original config when no override', () => {
    const result = applyOutputOverride(config, undefined);
    expect(result).toEqual(config);
  });

  it('overrides output format', () => {
    const result = applyOutputOverride(config, 'table');
    expect(result.output).toBe('table');
  });
});


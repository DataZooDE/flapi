import { describe, expect, it } from 'vitest';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { buildEndpointUrl } from '../../src/lib/url';
import { guidanceFor } from '../../src/lib/errors';
import { determineEndpointType } from '../../src/lib/render';
import { requestRows } from '../../src/commands/metrics';
import { resolveValidateDir } from '../../src/commands/config/validate';
import { PROJECT_TEMPLATES } from '../../src/commands/project/templates';
import { AxiosResponseError } from '@flapi/shared';

describe('#211 cluster E2 (CLI)', () => {
  it('keeps ":" literal and appends ?method=', () => {
    expect(buildEndpointUrl('/products/:id')).toBe('/api/v1/_config/endpoints/-products-:id');
    expect(buildEndpointUrl('/orders', 'reload', 'post')).toBe('/api/v1/_config/endpoints/-orders/reload?method=POST');
    expect(buildEndpointUrl('/orders', '', 'GET')).toBe('/api/v1/_config/endpoints/-orders?method=GET');
  });

  it('says "token rejected" when a token was sent', () => {
    const err = new AxiosResponseError({ status: 401, statusText: 'Unauthorized', data: {}, headers: {} } as any);
    expect(guidanceFor(err, undefined, true)).toContain('rejected the token');
    expect(guidanceFor(err, undefined, false)).toContain('needs a token');
  });

  it('shows REST+Tool for a file with url-path and mcp-tool', () => {
    expect(determineEndpointType({ urlPath: '/x', mcpTool: {} })).toBe('REST+Tool');
    expect(determineEndpointType({ mcpResource: {} })).toBe('MCP Resource');
    expect(determineEndpointType({ urlPath: '/x' })).toBe('REST');
  });

  it('shows p50 and rejected in the metrics table', () => {
    const [row] = requestRows([
      { protocol: 'rest', method: 'GET', route: '/a', requests: 2, rejected: 1, status: {}, latency_ms: { avg: 1, p50: 1, p95: 2, p99: 3, max: 3 } },
    ]);
    expect(row.p50_ms).toBe('~1.00');
    expect(row.rejected).toBe(1);
  });

  it('validates the template.path of --config', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'flapii-e2-'));
    writeFileSync(join(dir, 'flapi.yaml'), 'template:\n  path: ./endpoints\n');
    const resolved = await resolveValidateDir(undefined, join(dir, 'flapi.yaml'));
    expect(resolved.dir).toBe(join(dir, 'endpoints'));
    await expect(resolveValidateDir(undefined, join(dir, 'missing.yaml'))).rejects.toThrow();
    expect((await resolveValidateDir('x', undefined)).dir).toBe('x');
  });

  it('scaffolds includes that resolve from sqls/ and a usable auth block', () => {
    const templates = PROJECT_TEMPLATES as Record<string, string>;
    expect(templates['common/auth.yaml']).toContain('../common/auth.yaml');
    expect(templates['common/auth.yaml']).toMatch(/auth:\n  enabled: true/);
    expect(templates['.gitignore']).not.toMatch(/^data\/$/m);
    expect(templates['sqls/sample.sql']).not.toContain('LIMIT');
  });
});

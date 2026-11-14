// Regression tests for the #211 cluster-C CLI fixes.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { AxiosResponseError } from '@flapi/shared';
import { applyConfigJsonStyle, renderJson } from '../../src/lib/render';
import { cacheAuditRows, describeRefreshResult, refreshQuery } from '../../src/lib/cacheRender';
import { handleError, messageFromBody } from '../../src/lib/errors';
import { loadConfig } from '../../src/lib/config';
import { PROJECT_TEMPLATES } from '../../src/commands/project/templates';
import { requestRows } from '../../src/commands/metrics';

const mockInfo = vi.fn();
const mockError = vi.fn();

vi.mock('../../src/lib/console', () => ({
  Console: {
    info: (...args: unknown[]) => mockInfo(...args),
    error: (...args: unknown[]) => mockError(...args),
    warn: vi.fn(),
    success: vi.fn(),
    spinner: vi.fn(),
    color: vi.fn((_, str: string) => str),
  },
}));

const originalEnv = { ...process.env };

beforeEach(() => {
  mockInfo.mockReset();
  mockError.mockReset();
  process.env = { ...originalEnv };
});
afterEach(() => {
  process.env = { ...originalEnv };
});

describe('--json-style only re-cases config field names', () => {
  it('never touches data payloads', () => {
    const metrics = { requests: { series: [{ route: '/customers', status: { '2xx': 3, '5xx': 0 } }] }, server_errors: 0 };
    renderJson(metrics);
    expect(JSON.parse(mockInfo.mock.calls[0][0])).toEqual(metrics);
  });

  it('keeps endpoint-path keys of a keyed document and re-cases their fields', () => {
    const endpoints = { '/customers': { 'url-path': '/customers', 'template-source': 'c.sql' } };
    expect(applyConfigJsonStyle(endpoints, 'camel', 'keyed')).toEqual({
      '/customers': { urlPath: '/customers', templateSource: 'c.sql' },
    });
    expect(applyConfigJsonStyle({ '/a-b': { urlPath: '/a-b' } }, 'hyphen', 'keyed')).toEqual({
      '/a-b': { 'url-path': '/a-b' },
    });
  });

  it('keeps user-named children (schema properties, parameters) verbatim', () => {
    const tool = { 'mcp-tool': { name: 't', 'input-schema': { properties: { customer_id: { type: 'integer' } } } } };
    const out = applyConfigJsonStyle(tool, 'camel') as any;
    expect(out.mcpTool.inputSchema).toEqual({ properties: { customer_id: { type: 'integer' } } });
  });

  it('re-cases a list of documents', () => {
    expect(applyConfigJsonStyle([{ 'url-path': '/x' }], 'camel')).toEqual([{ urlPath: '/x' }]);
  });
});

describe('error bodies are printed', () => {
  const responseError = (status: number, statusText: string, data: unknown) =>
    new AxiosResponseError({ status, statusText, data, headers: {}, config: {} as any } as any);

  it('prints the plain-text reason, not just "Bad Request"', () => {
    handleError(responseError(400, 'Bad Request', 'Cache is not enabled for this endpoint'));
    expect(mockError).toHaveBeenCalledWith('[400] Bad Request: Cache is not enabled for this endpoint');
  });

  it('prints the error field of a JSON body, then the body', () => {
    handleError(responseError(400, 'Bad Request', { error: 'Binder Error: no column x' }));
    expect(mockError).toHaveBeenNthCalledWith(1, '[400] Bad Request: Binder Error: no column x');
    expect(mockError).toHaveBeenNthCalledWith(2, JSON.stringify({ error: 'Binder Error: no column x' }, null, 2));
  });

  it('finds message/error/reason', () => {
    expect(messageFromBody({ reason: 'r' })).toBe('r');
    expect(messageFromBody({ error: { message: 'nested' } })).toBe('nested');
    expect(messageFromBody('')).toBeUndefined();
  });
});

describe('cache refresh and audit', () => {
  it('--full sends full=true; --force alone is a normal refresh', () => {
    expect(refreshQuery({ full: true })).toEqual({ full: 'true' });
    expect(refreshQuery({ force: true })).toBeUndefined();
    expect(refreshQuery({})).toBeUndefined();
  });

  it('describes a refresh result, and nothing for an empty body', () => {
    expect(describeRefreshResult('')).toEqual([]);
    expect(describeRefreshResult({ mode: 'merge', rows_affected: 4, duration_ms: 12, snapshot_id: 7 })).toEqual([
      'Mode: merge',
      'Rows: 4',
      'Duration: 12 ms',
      'Snapshot: 7',
    ]);
  });

  it('reduces audit rows to the table columns', () => {
    const rows = cacheAuditRows(
      [{ endpoint_path: '/a', sync_started_at: 't0', sync_type: 'full', status: 'success', duration_ms: 5, rows_affected: 3, snapshot_id: 9, message: 'x' }],
      true,
    );
    expect(rows).toEqual([{ endpoint: '/a', time: 't0', mode: 'full', status: 'success', duration_ms: 5, rows: 3, snapshot: 9 }]);
    expect(Object.keys(cacheAuditRows([{ sync_type: 'full' }])[0])).not.toContain('endpoint');
    expect(cacheAuditRows({ not: 'a list' })).toEqual([]);
  });
});

describe('environment', () => {
  it('accepts FLAPI_URL when FLAPI_BASE_URL is unset', () => {
    delete process.env.FLAPI_BASE_URL;
    process.env.FLAPI_URL = 'http://plugin-host:9000';
    expect(loadConfig({}).baseUrl).toBe('http://plugin-host:9000');
  });

  it('prefers FLAPI_BASE_URL over FLAPI_URL', () => {
    process.env.FLAPI_BASE_URL = 'http://base';
    process.env.FLAPI_URL = 'http://plugin';
    expect(loadConfig({}).baseUrl).toBe('http://base');
  });
});

describe('project init', () => {
  it('ships the sample data the sample endpoint reads', () => {
    expect(PROJECT_TEMPLATES['flapi.yaml']).toContain("path: './data/sample.csv'");
    expect(PROJECT_TEMPLATES['data/sample.csv']).toMatch(/^id,name/);
    expect(PROJECT_TEMPLATES['sqls/sample.sql']).toContain('read_csv_auto');
  });
});

describe('metrics', () => {
  it('shows cache-not-ready 503s apart from 5xx', () => {
    const rows = requestRows([
      {
        protocol: 'rest', method: 'GET', route: '/c', requests: 5,
        status: { '2xx': 3, '3xx': 0, '4xx': 0, '5xx': 0, '503_cache': 2 },
        latency_ms: { avg: 1, p50: 1, p95: 1, p99: 1, max: 1 },
      },
    ]);
    expect(rows[0].errors).toBe('0 4xx / 0 5xx / 2 cache-not-ready');
  });
});

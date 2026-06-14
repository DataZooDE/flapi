import { describe, it, expect } from 'vitest';
import { buildEndpointPickItems } from '../src/webview/endpointPick';

describe('buildEndpointPickItems', () => {
  it('labels REST endpoints with method and path (hyphen keys)', () => {
    const items = buildEndpointPickItems([
      { 'url-path': '/customers', method: 'get', 'template-source': 'customers.sql' },
    ]);
    expect(items[0].label).toBe('GET /customers');
    expect(items[0].description).toBe('customers.sql');
  });

  it('supports camelCased keys', () => {
    const items = buildEndpointPickItems([
      { urlPath: '/orders', method: 'POST', templateSource: 'orders.sql' },
    ]);
    expect(items[0].label).toBe('POST /orders');
    expect(items[0].description).toBe('orders.sql');
  });

  it('falls back to an MCP name when there is no url path or method', () => {
    const items = buildEndpointPickItems([{ mcp_name: 'flight_list' }]);
    expect(items[0].label).toBe('flight_list');
  });

  it('uses a placeholder when nothing identifies the endpoint', () => {
    const items = buildEndpointPickItems([{}]);
    expect(items[0].label).toBe('(unnamed)');
  });

  it('carries the original config through on each item', () => {
    const cfg = { 'url-path': '/x', method: 'GET' };
    expect(buildEndpointPickItems([cfg])[0].config).toBe(cfg);
  });
});

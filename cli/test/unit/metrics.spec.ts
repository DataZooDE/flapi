import { describe, it, expect } from 'vitest';
import { requestRows, cacheRows, type RequestSeries, type CacheSeries } from '../../src/commands/metrics';

const lat = (p95: number) => ({ avg: p95 / 2, p50: p95 / 2, p95, p99: p95, max: p95 });

describe('metrics rows', () => {
  it('orders request series slowest p95 first and labels REST and MCP', () => {
    const series: RequestSeries[] = [
      { protocol: 'rest', method: 'GET', route: '/fast', requests: 10, status: { '2xx': 10, '3xx': 0, '4xx': 0, '5xx': 0 }, latency_ms: lat(1) },
      { protocol: 'mcp', tool: 'customer_lookup', requests: 3, failures: 1, latency_ms: lat(50) },
    ];
    const rows = requestRows(series);
    expect(rows[0].endpoint).toBe('tool customer_lookup');
    expect(rows[0].errors).toBe(1);
    expect(rows[1].endpoint).toBe('GET /fast');
    expect(rows[1].errors).toBe('0 4xx / 0 5xx');
    expect(rows[1].p95_ms).toBe('~1.00');
  });

  it('orders caches by last refresh duration and shows never-refreshed caches', () => {
    const caches: CacheSeries[] = [
      { cache: 'cache.main.a', state: 'unbuilt' },
      { cache: 'cache.main.b', state: 'ready', refreshes: 2, failures: 0, last: { mode: 'merge', status: 'success', duration_ms: 900, rows_affected: 12 }, duration_ms: { avg: 800, max: 900 }, data_age_seconds: 30 },
    ];
    const rows = cacheRows(caches);
    expect(rows[0].cache).toBe('cache.main.b');
    expect(rows[0].mode).toBe('merge');
    expect(rows[0].refreshes).toBe('2 (0 failed)');
    expect(rows[1].last_status).toBe('never refreshed');
  });
});

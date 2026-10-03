import { describe, it, expect } from 'vitest';
import { flapii } from './helpers';

// /customers/ is the cache-enabled REST endpoint of the shipped examples. These
// used to pick "the first endpoint" and return silently when there was none, so
// they could pass without exercising anything.
describe('cli cache command (integration)', () => {
  it('lists cache configurations', async () => {
    const r = await flapii(['cache', 'list', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout.length).toBeGreaterThan(0);
  });

  it('gets the cache configuration of a cached endpoint', async () => {
    const r = await flapii(['cache', 'get', '/customers/', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    const cache = JSON.parse(r.stdout);
    expect(cache.enabled ?? cache.cache?.enabled).toBe(true);
  });

  it('refreshes the cache of a cached endpoint', async () => {
    const r = await flapii(['cache', 'refresh', '/customers/', '--force', '--output', 'json']);
    expect(r.exitCode, r.stderr + r.stdout).toBe(0);
    expect(r.stdout.length).toBeGreaterThan(0);
  });
});

import { describe, it, expect } from 'vitest';
import { flapii } from './helpers';

// Deterministic: /customers/ is a REST endpoint of the shipped examples. These used
// to take "the first key of the list" and return when there was none, so they
// depended on listing order and could pass without testing anything.
describe('cli templates command (integration)', () => {
  it('lists templates', async () => {
    const r = await flapii(['templates', 'list', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout.length).toBeGreaterThan(0);
  });

  it('gets a template', async () => {
    const r = await flapii(['templates', 'get', '/customers/', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(typeof JSON.parse(r.stdout).template).toBe('string');
  });

  it('tests a template', async () => {
    const r = await flapii(['templates', 'test', '/customers/', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(JSON.parse(r.stdout)).toHaveProperty('success', true);
  });
});

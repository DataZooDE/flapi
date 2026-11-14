import { describe, it, expect } from 'vitest';
import { reloadEndpointConfig } from '@flapi/shared';

// A stub with just the post() the helper calls.
const clientAnswering = (status: number, data: unknown) =>
  ({ post: async () => ({ status, data }) }) as never;

describe('reloadEndpointConfig error text', () => {
  it('uses the server error when an ambiguous slug answers 400 with only {error}', async () => {
    const msg = 'Ambiguous: /tasks is served by GET (a.yaml), POST (b.yaml); specify ?method=GET|POST';
    const result = await reloadEndpointConfig(clientAnswering(400, { error: msg }), '/tasks');
    expect(result.success).toBe(false);
    expect(result.message).toBe(msg);
  });

  it('keeps message and reason when the server sends both', async () => {
    const result = await reloadEndpointConfig(
      clientAnswering(400, { success: false, message: 'Reload failed', reason: 'bad yaml', error: 'Reload failed' }),
      '/tasks',
    );
    expect(result.message).toBe('Reload failed: bad yaml');
  });
});

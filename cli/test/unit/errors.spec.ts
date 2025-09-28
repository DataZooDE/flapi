import { describe, it, expect, vi, beforeEach } from 'vitest';
import { handleError } from '../../src/lib/errors';
import { AxiosError } from 'axios';

const mockError = vi.fn();

vi.mock('../../src/lib/console', () => ({
  Console: {
    error: (...args: unknown[]) => mockError(...args),
  },
}));

describe('handleError', () => {
  beforeEach(() => {
    mockError.mockReset();
  });

  it('logs axios error status and message', () => {
    const error = new AxiosError('Request failed', 'ERR_BAD_REQUEST', undefined, undefined, {
      status: 409,
      statusText: 'Conflict',
      headers: {},
      config: {},
      data: { message: 'Endpoint already exists' },
    } as any);

    handleError(error);
    expect(mockError).toHaveBeenNthCalledWith(1, '[409] Conflict: Endpoint already exists');
    expect(mockError).toHaveBeenNthCalledWith(2, JSON.stringify({ message: 'Endpoint already exists' }, null, 2));
  });

  it('logs generic error message', () => {
    handleError(new Error('boom'));
    expect(mockError).toHaveBeenCalledWith('boom');
  });
});

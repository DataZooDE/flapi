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
    // Once: the body says nothing the one-line message did not.
    expect(mockError).toHaveBeenCalledTimes(1);
    expect(mockError).toHaveBeenNthCalledWith(1, '[409] Conflict: Endpoint already exists');
  });

  it('still prints a body that carries more than the message', () => {
    const data = { error: 'Validation failed', errors: [{ field: 'id', message: 'must be an int' }] };
    const error = new AxiosError('Request failed', 'ERR_BAD_REQUEST', undefined, undefined, {
      status: 400,
      statusText: 'Bad Request',
      headers: {},
      config: {},
      data,
    } as any);

    handleError(error);
    expect(mockError).toHaveBeenNthCalledWith(1, '[400] Bad Request: Validation failed');
    expect(mockError).toHaveBeenNthCalledWith(2, JSON.stringify(data, null, 2));
  });

  it('quiet reports the failure in one line', () => {
    const error = new AxiosError('Request failed', 'ERR_BAD_REQUEST', undefined, undefined, {
      status: 404,
      statusText: 'Not Found',
      headers: {},
      config: {},
      data: { error: 'Endpoint not found: /x', hint: 'list endpoints' },
    } as any);

    handleError(error, { quiet: true });
    expect(mockError).toHaveBeenCalledTimes(1);
    expect(mockError).toHaveBeenCalledWith('[404] Not Found: Endpoint not found: /x');
  });

  it('logs generic error message', () => {
    handleError(new Error('boom'));
    expect(mockError).toHaveBeenCalledWith('boom');
  });
});

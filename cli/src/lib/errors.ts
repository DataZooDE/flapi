import axios, { AxiosError } from 'axios';
import { AxiosResponseError } from '@flapi/shared';
import { Console } from './console';

export class ApiError extends Error {
  constructor(message: string, public readonly status?: number) {
    super(message);
    this.name = 'ApiError';
  }
}

export class ConflictError extends ApiError {
  constructor(message: string) {
    super(message, 409);
    this.name = 'ConflictError';
  }
}

const TOKEN_HELP =
  'The config service needs a token. Start flAPI with `--config-service --config-service-token <token>`, ' +
  'and give the same token to flapii with --config-service-token or the FLAPI_CONFIG_SERVICE_TOKEN environment variable.';

/** What to do about a failure a first-time user is likely to hit, if anything. */
export function guidanceFor(error: unknown, baseUrl?: string): string | undefined {
  if (error instanceof AxiosResponseError) {
    const status = error.response.status;
    const body = typeof error.response.data === 'string' ? error.response.data : '';
    if (status === 401 || status === 403) {
      return `${TOKEN_HELP} The server rejected the token (HTTP ${status}).`;
    }
    // A disabled config service has no routes at all, so the generic router 404 is
    // all there is; a real "endpoint not found" carries its own message.
    if (status === 404 && /^\s*not found\s*$/i.test(body)) {
      return 'The config service may be disabled on this server. Start flAPI with `--config-service --config-service-token <token>`.';
    }
    return undefined;
  }
  if (isAxiosError(error) && !error.response) {
    const where = baseUrl ?? error.config?.baseURL ?? 'the configured URL';
    return `Cannot reach flAPI at ${where}. Is it running? Set the address with --base-url or FLAPI_BASE_URL.`;
  }
  return undefined;
}

export function handleError(error: unknown, opts: { quiet?: boolean; baseUrl?: string } = {}): void {
  if (opts.quiet) {
    process.exitCode = 1;
    return;
  }

  const guidance = guidanceFor(error, opts.baseUrl);
  if (guidance) {
    Console.error(guidance);
  }

  if (isAxiosError(error)) {
    const status = error.response?.status;
    const statusText = error.response?.statusText ?? 'HTTP Error';
    const detail = extractMessage(error);

    const prefix = status ? `[${status}] ${statusText}` : statusText;
    Console.error(`${prefix}: ${detail}`);

    if (error.response?.data && typeof error.response.data === 'object') {
      Console.error(JSON.stringify(error.response.data, null, 2));
    }
    return;
  }

  // What the shared HTTP client throws for every non-2xx answer. Its message is
  // only the status text, so this branch used to fall through to the generic
  // one below and `templates test` / `cache refresh` printed just "Bad
  // Request" - the server's reason (a DuckDB error, "Cache is not enabled")
  // was in the body and never shown.
  if (error instanceof AxiosResponseError) {
    const { status, statusText, data } = error.response;
    const prefix = `[${status}] ${statusText || 'HTTP Error'}`;
    const detail = messageFromBody(data);
    Console.error(detail ? `${prefix}: ${detail}` : prefix);
    if (data && typeof data === 'object') {
      Console.error(JSON.stringify(data, null, 2));
    }
    return;
  }

  if (error instanceof ApiError) {
    Console.error(error.message);
    return;
  }

  if (error instanceof Error) {
    Console.error(error.message);
    return;
  }

  Console.error('Unknown error occurred');
}

export function isAxiosError(error: unknown): error is AxiosError {
  return axios.isAxiosError(error);
}

/** The human-readable reason in an error body: a plain-text body, or its message/error/reason field. */
export function messageFromBody(data: unknown): string | undefined {
  if (typeof data === 'string') {
    const trimmed = data.trim();
    return trimmed.length > 0 ? trimmed : undefined;
  }
  if (data && typeof data === 'object') {
    const body = data as Record<string, unknown>;
    for (const key of ['error', 'message', 'reason']) {
      const value = body[key];
      if (typeof value === 'string' && value.length > 0) {
        return value;
      }
      if (value && typeof value === 'object') {
        const nested = (value as Record<string, unknown>).message;
        if (typeof nested === 'string' && nested.length > 0) {
          return nested;
        }
      }
    }
  }
  return undefined;
}

function extractMessage(error: AxiosError): string {
  const data = error.response?.data as { message?: string; error?: string } | undefined;
  return data?.message ?? data?.error ?? error.message;
}


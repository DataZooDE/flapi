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

const TOKEN_REJECTED_HELP =
  'The config service rejected the token flapii sent. It must equal the server\'s --config-service-token; ' +
  'check --config-service-token / FLAPI_CONFIG_SERVICE_TOKEN (or FLAPI_TOKEN) against the token the server was started with.';

/**
 * What to do about a failure a first-time user is likely to hit, if anything.
 * `tokenSent`: whether this request carried a token - a 401 then means the
 * token is WRONG, and "the config service needs a token" sent people looking
 * for a flag they had already passed.
 */
export function guidanceFor(error: unknown, baseUrl?: string, tokenSent?: boolean): string | undefined {
  if (error instanceof AxiosResponseError) {
    const status = error.response.status;
    const body = typeof error.response.data === 'string' ? error.response.data : '';
    if (status === 401 || status === 403) {
      if (tokenSent) {
        return `${TOKEN_REJECTED_HELP} (HTTP ${status})`;
      }
      return `${TOKEN_HELP} No token was sent (HTTP ${status}).`;
    }
    // A disabled config service has no routes at all, so the generic router 404 is
    // all there is; a real "endpoint not found" carries its own message.
    if (status === 404 && /^\s*not found\s*$/i.test(body)) {
      return 'The config service may be disabled on this server. Start flAPI with `--config-service --config-service-token <token>`.';
    }
    return undefined;
  }
  if (isAxiosError(error) && !error.response && isTimeout(error)) {
    // The server answered nothing within --timeout - which on a cache refresh or
    // a slow template test means it is still WORKING, not down. "Is it running?"
    // sent people restarting a server mid-refresh.
    const ms = typeof error.config?.timeout === 'number' ? error.config.timeout : undefined;
    const after = ms ? `after ${Math.round(ms / 1000)}s` : 'before the server answered';
    return `Request timed out ${after}; the server may still be working (a refresh keeps running). ` +
      'Raise --timeout (or FLAPI_TIMEOUT) and check `flapii cache audit <path>`.';
  }
  if (isAxiosError(error) && !error.response) {
    const where = baseUrl ?? error.config?.baseURL ?? 'the configured URL';
    return `Cannot reach flAPI at ${where}. Is it running? Set the address with --base-url or FLAPI_BASE_URL.`;
  }
  return undefined;
}

export function handleError(
  error: unknown,
  opts: { quiet?: boolean; baseUrl?: string; authToken?: string; configServiceToken?: string } = {},
): void {
  // Quiet still reports the failure - once, one line, no guidance - so a
  // script sees why it failed; it used to print nothing at all while the
  // spinner's own "Failed to ..." line still came out.
  process.exitCode = 1;
  if (opts.quiet) {
    Console.error(oneLineError(error));
    return;
  }

  const tokenSent = Boolean(opts.authToken ?? opts.configServiceToken ?? process.env.FLAPI_CONFIG_SERVICE_TOKEN ?? process.env.FLAPI_TOKEN);
  const guidance = guidanceFor(error, opts.baseUrl, tokenSent);
  if (guidance) {
    Console.error(guidance);
  }

  if (isAxiosError(error)) {
    const status = error.response?.status;
    const statusText = error.response?.statusText ?? 'HTTP Error';
    const detail = extractMessage(error);

    const prefix = status ? `[${status}] ${statusText}` : statusText;
    Console.error(`${prefix}: ${detail}`);
    printBodyIfMoreThan(error.response?.data, detail);
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
    printBodyIfMoreThan(data, detail);
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

/**
 * The error body as JSON, unless it says nothing the one-line message did not:
 * `{"error": "Endpoint not found: /x"}` used to be printed a second time, in
 * full, under "[404] Not Found: Endpoint not found: /x".
 */
export function printBodyIfMoreThan(data: unknown, detail: string | undefined): void {
  if (!data || typeof data !== 'object') {
    return;
  }
  const body = data as Record<string, unknown>;
  const said = (key: string) =>
    key === 'success' || (['error', 'message', 'reason'].includes(key) && body[key] === detail);
  if (Object.keys(body).some((key) => !said(key))) {
    Console.error(JSON.stringify(data, null, 2));
  }
}

/** The failure in one line: the status and the server's reason. */
export function oneLineError(error: unknown): string {
  if (error instanceof AxiosResponseError) {
    const { status, statusText, data } = error.response;
    const detail = messageFromBody(data);
    const prefix = `[${status}] ${statusText || 'HTTP Error'}`;
    return detail ? `${prefix}: ${detail}` : prefix;
  }
  if (isAxiosError(error)) {
    const status = error.response?.status;
    const detail = extractMessage(error);
    return status ? `[${status}] ${error.response?.statusText ?? 'HTTP Error'}: ${detail}` : detail;
  }
  if (error instanceof Error) {
    return error.message;
  }
  return 'Unknown error occurred';
}

export function isAxiosError(error: unknown): error is AxiosError {
  return axios.isAxiosError(error);
}

/** Axios reports its own `timeout` as ECONNABORTED ("timeout of Nms exceeded") or ETIMEDOUT. */
function isTimeout(error: AxiosError): boolean {
  return error.code === 'ECONNABORTED' || error.code === 'ETIMEDOUT' || /timeout of \d+ms exceeded/i.test(error.message);
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


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

function extractMessage(error: AxiosError): string {
  const data = error.response?.data as { message?: string; error?: string } | undefined;
  return data?.message ?? data?.error ?? error.message;
}


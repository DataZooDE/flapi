import axios, { AxiosError } from 'axios';
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

export function handleError(error: unknown, opts: { quiet?: boolean } = {}): void {
  if (opts.quiet) {
    process.exitCode = 1;
    return;
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


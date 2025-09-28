import { AxiosError } from 'axios';
import chalk from 'chalk';
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
    const responseData = error.response?.data;
    
    // Show status and basic error
    Console.error(`${status ? `[${status}] ` : ''}${statusText}`);
    
    // Show detailed server response
    if (responseData) {
      if (typeof responseData === 'string') {
        // Server returned plain text error
        Console.error(chalk.red(`Server Error: ${responseData}`));
      } else if (typeof responseData === 'object') {
        // Server returned JSON error
        const detail = extractMessage(error);
        Console.error(chalk.red(`Error: ${detail}`));
        Console.error(chalk.gray('Full response:'));
        Console.error(JSON.stringify(responseData, null, 2));
      }
    } else {
      // Fallback to error message
      Console.error(chalk.red(`Error: ${error.message}`));
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
  return !!error && typeof error === 'object' && 'isAxiosError' in error;
}

function extractMessage(error: AxiosError): string {
  const data = error.response?.data as { message?: string; error?: string } | undefined;
  return data?.message ?? data?.error ?? error.message;
}


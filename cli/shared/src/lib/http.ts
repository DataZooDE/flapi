import https from 'node:https';
import axios, { AxiosError } from 'axios';
import axiosRetry from 'axios-retry';
import type { ApiClientConfig } from './types';

// Retried: reads only. A lost response to a POST/PUT/DELETE may mean the server
// already applied it, and repeating `endpoints create`, a cache refresh or a
// delete is a different operation, not a retry.
const SAFE_METHODS = new Set(['get', 'head', 'options']);
const RETRYABLE_STATUSES = new Set([429, 500, 502, 503, 504]);

export function createApiClient(config: ApiClientConfig) {
  const headers: Record<string, string> = {
    Accept: 'application/json',
    ...config.headers,
  };

  // The config service accepts both; X-Config-Token is preferred, Bearer is the
  // backward-compatible form.
  if (config.authToken) {
    headers['X-Config-Token'] = config.authToken;
    headers['Authorization'] = `Bearer ${config.authToken}`;
  }

  const instance = axios.create({
    baseURL: config.baseUrl.replace(/\/$/, ''),
    timeout: config.timeout * 1000,
    headers,
    httpsAgent: config.verifyTls ? undefined : new https.Agent({ rejectUnauthorized: false }),
  });

  // Registered BEFORE the error translation below, so it sees the real AxiosError
  // (with `response`) and the status-based retries actually happen. With
  // `validateStatus: () => true` an HTTP error never reached it at all.
  axiosRetry(instance, {
    retries: config.retries,
    retryDelay: axiosRetry.exponentialDelay,
    retryCondition: (error) => {
      const method = (error.config?.method ?? 'get').toLowerCase();
      if (!SAFE_METHODS.has(method)) return false;
      if (!error.response) return true; // network error
      return RETRYABLE_STATUSES.has(error.response.status);
    },
  });

  // Callers (handleError and friends) expect a thrown AxiosResponseError for any
  // non-2xx answer.
  instance.interceptors.response.use(
    (response) => response,
    (error: unknown) => {
      if (axios.isAxiosError(error) && error.response) {
        throw new AxiosResponseError(error.response);
      }
      throw error;
    },
  );

  return instance;
}

export class AxiosResponseError extends Error {
  constructor(public readonly response: import('axios').AxiosResponse) {
    super(response.statusText || `HTTP ${response.status}`);
    this.name = 'AxiosResponseError';
  }
}

export function isAxiosError(error: unknown): error is AxiosError {
  return axios.isAxiosError(error);
}

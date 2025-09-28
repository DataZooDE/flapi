import https from 'node:https';
import axios, { AxiosError } from 'axios';
import axiosRetry from 'axios-retry';
import type { ApiClientConfig } from './types';

export function createApiClient(config: ApiClientConfig) {
  const instance = axios.create({
    baseURL: config.baseUrl.replace(/\/$/, ''),
    timeout: config.timeout * 1000,
    headers: {
      Accept: 'application/json',
      ...config.headers,
      ...(config.authToken ? { Authorization: `Bearer ${config.authToken}` } : {}),
    },
    validateStatus: () => true,
    httpsAgent: config.verifyTls ? undefined : new https.Agent({ rejectUnauthorized: false }),
  });

  axiosRetry(instance, {
    retries: config.retries,
    retryDelay: axiosRetry.exponentialDelay,
    retryCondition: (error) => {
      if (!error.response) return true;
      const { status } = error.response;
      return [429, 500, 502, 503, 504].includes(status);
    },
  });

  instance.interceptors.response.use((response) => {
    if (response.status >= 200 && response.status < 300) {
      return response;
    }
    throw new AxiosResponseError(response);
  });

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


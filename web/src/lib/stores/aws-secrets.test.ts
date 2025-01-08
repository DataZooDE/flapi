import { describe, it, expect, vi, beforeEach } from 'vitest';
import { awsSecretsStore } from './aws-secrets';
import { api } from '../api';
import { get } from 'svelte/store';
import type { AwsSecretsConfig } from '$lib/types';

// Mock the entire api object
vi.mock('../api', () => ({
  api: {
    getAwsSecretsConfig: vi.fn(),
    updateAwsSecretsConfig: vi.fn(),
    testAwsSecretsManagerConnection: vi.fn()
  }
}));

describe('awsSecretsStore', () => {
  const mockConfig: AwsSecretsConfig = {
    secret_name: 'test-secret',
    region: 'us-east-1',
    credentials: {
      secret_id: 'test-id',
      secret_key: 'test-key'
    },
    init_sql: 'SELECT * FROM users',
    secret_table: 'auth_users'
  };

  beforeEach(() => {
    vi.clearAllMocks();
    awsSecretsStore.reset();
  });

  it('should initialize with default state', () => {
    const state = get(awsSecretsStore);
    expect(state).toEqual({
      data: null,
      loading: false,
      error: null
    });
  });

  it('should load AWS Secrets config', async () => {
    // Use vi.fn() directly since we're mocking the entire module
    api.getAwsSecretsConfig.mockResolvedValue(mockConfig);

    await awsSecretsStore.load();
    const state = get(awsSecretsStore);

    expect(state.data).toEqual(mockConfig);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
  });

  it('should save AWS Secrets config', async () => {
    await awsSecretsStore.save(mockConfig);
    const state = get(awsSecretsStore);

    expect(state.data).toEqual(mockConfig);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(api.updateAwsSecretsConfig).toHaveBeenCalledWith(mockConfig);
  });

  it('should test AWS Secrets connection', async () => {
    await awsSecretsStore.testConnection(mockConfig);
    const state = get(awsSecretsStore);

    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(api.testAwsSecretsManagerConnection).toHaveBeenCalledWith(mockConfig);
  });

  it('should handle load errors', async () => {
    const error = new Error('Failed to load');
    api.getAwsSecretsConfig.mockRejectedValue(error);

    try {
      await awsSecretsStore.load();
    } catch (e) {
      // Expected error
    }

    const state = get(awsSecretsStore);
    expect(state.error).toBe('Failed to load');
    expect(state.loading).toBe(false);
  });

  it('should handle save errors', async () => {
    const error = new Error('Failed to save');
    api.updateAwsSecretsConfig.mockRejectedValue(error);

    try {
      await awsSecretsStore.save(mockConfig);
    } catch (e) {
      // Expected error
    }

    const state = get(awsSecretsStore);
    expect(state.error).toBe('Failed to save');
    expect(state.loading).toBe(false);
  });

  it('should handle test connection errors', async () => {
    const error = new Error('Connection failed');
    api.testAwsSecretsManagerConnection.mockRejectedValue(error);

    try {
      await awsSecretsStore.testConnection(mockConfig);
    } catch (e) {
      // Expected error
    }

    const state = get(awsSecretsStore);
    expect(state.error).toBe('Connection failed');
    expect(state.loading).toBe(false);
  });
}); 
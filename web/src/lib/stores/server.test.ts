import { describe, it, expect, vi, beforeEach } from 'vitest';
import { serverStore } from './server';
import { api } from '../api';
import { get } from 'svelte/store';
import type { ServerConfig } from '$lib/types';

// Mock the entire api object
vi.mock('../api', () => ({
  api: {
    getServerConfig: vi.fn(),
    updateServerConfig: vi.fn()
  }
}));

describe('serverStore', () => {
  const mockConfig: ServerConfig = {
    name: 'test-server',
    http_port: 8080,
    cache_schema: 'cache'
  };

  beforeEach(() => {
    vi.clearAllMocks();
    serverStore.reset();
  });

  it('should initialize with default state', () => {
    const state = get(serverStore);
    expect(state).toEqual({
      data: null,
      loading: false,
      error: null
    });
  });

  it('should load server config', async () => {
    // Use vi.fn() directly since we're mocking the entire module
    api.getServerConfig.mockResolvedValue(mockConfig);

    await serverStore.load();
    const state = get(serverStore);

    expect(state.data).toEqual(mockConfig);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
  });

  it('should save server config', async () => {
    await serverStore.save(mockConfig);
    const state = get(serverStore);

    expect(state.data).toEqual(mockConfig);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(api.updateServerConfig).toHaveBeenCalledWith(mockConfig);
  });

  it('should handle load errors', async () => {
    const error = new Error('Failed to load');
    api.getServerConfig.mockRejectedValue(error);

    try {
      await serverStore.load();
    } catch (e) {
      // Expected error
    }

    const state = get(serverStore);
    expect(state.error).toBe('Failed to load');
    expect(state.loading).toBe(false);
  });

  it('should handle save errors', async () => {
    const error = new Error('Failed to save');
    api.updateServerConfig.mockRejectedValue(error);

    try {
      await serverStore.save(mockConfig);
    } catch (e) {
      // Expected error
    }

    const state = get(serverStore);
    expect(state.error).toBe('Failed to save');
    expect(state.loading).toBe(false);
  });
}); 
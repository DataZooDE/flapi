import { describe, it, expect, vi, beforeEach } from 'vitest';
import { connectionsStore } from './connections';
import { api } from '../api';
import { get } from 'svelte/store';
import type { ConnectionConfig } from '$lib/types';

// Mock the entire api object
vi.mock('../api', () => ({
  api: {
    getConnection: vi.fn(),
    updateConnection: vi.fn(),
    testConnection: vi.fn()
  }
}));

describe('connectionsStore', () => {
  const mockConfig: ConnectionConfig = {
    type: 'postgres',
    properties: {
      host: 'localhost',
      port: '5432'
    }
  };

  beforeEach(() => {
    vi.clearAllMocks();
    connectionsStore.reset();
  });

  it('should load connection configuration', async () => {
    // Use vi.fn() directly since we're mocking the entire module
    api.getConnection.mockResolvedValue(mockConfig);

    await connectionsStore.load('test');
    const state = get(connectionsStore);

    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(state.data).toEqual({ test: mockConfig });
  });

  it('should handle load errors', async () => {
    const error = new Error('Failed to load');
    api.getConnection.mockRejectedValue(error);

    try {
      await connectionsStore.load('test');
    } catch (e) {
      // Expected error
    }

    const state = get(connectionsStore);
    expect(state.loading).toBe(false);
    expect(state.error).toBe('Failed to load');
    expect(state.data).toBeNull();
  });

  it('should save connection configuration', async () => {
    await connectionsStore.save('test', mockConfig);
    const state = get(connectionsStore);

    expect(api.updateConnection).toHaveBeenCalledWith('test', mockConfig);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(state.data).toEqual({ test: mockConfig });
  });
}); 
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { duckdbStore } from './duckdb';
import { api } from '../api';
import { get } from 'svelte/store';

// Mock the entire api object
vi.mock('../api', () => ({
  api: {
    getDuckDBSettings: vi.fn(),
    updateDuckDBSettings: vi.fn()
  }
}));

describe('duckdbStore', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    duckdbStore.reset();
  });

  it('should initialize with default state', () => {
    const state = get(duckdbStore);
    expect(state).toEqual({
      data: null,
      loading: false,
      error: null
    });
  });

  it('should load DuckDB settings', async () => {
    const mockSettings = {
      settings: { key: 'value' },
      db_path: './test.db'
    };

    // Use vi.fn() directly since we're mocking the entire module
    api.getDuckDBSettings.mockResolvedValue(mockSettings);

    await duckdbStore.load();
    const state = get(duckdbStore);

    expect(state.data).toEqual(mockSettings);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
  });

  it('should handle load errors', async () => {
    const error = new Error('Failed to load');
    vi.mocked(api.getDuckDBSettings).mockRejectedValue(error);

    try {
      await duckdbStore.load();
    } catch (e) {
      // Expected error
    }

    const state = get(duckdbStore);
    expect(state.error).toBe('Failed to load');
    expect(state.loading).toBe(false);
  });

  it('should save DuckDB settings', async () => {
    const mockSettings = {
      settings: { key: 'value' },
      db_path: './test.db'
    };

    await duckdbStore.save(mockSettings);
    const state = get(duckdbStore);

    expect(state.data).toEqual(mockSettings);
    expect(state.loading).toBe(false);
    expect(state.error).toBe(null);
    expect(api.updateDuckDBSettings).toHaveBeenCalledWith(mockSettings);
  });

  it('should handle save errors', async () => {
    const error = new Error('Failed to save');
    vi.mocked(api.updateDuckDBSettings).mockRejectedValue(error);

    const mockSettings = {
      settings: { key: 'value' },
      db_path: './test.db'
    };

    try {
      await duckdbStore.save(mockSettings);
    } catch (e) {
      // Expected error
    }

    const state = get(duckdbStore);
    expect(state.error).toBe('Failed to save');
    expect(state.loading).toBe(false);
  });
}); 
import { writable } from 'svelte/store';
import { api } from '../api';

export interface DuckDBState {
  data: {
    settings: Record<string, string>;
    db_path: string;
  } | null;
  loading: boolean;
  error: string | null;
}

const initialState: DuckDBState = {
  data: null,
  loading: false,
  error: null
};

function createDuckDBStore() {
  const { subscribe, set, update } = writable<DuckDBState>(initialState);

  return {
    subscribe,
    async load() {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        const settings = await api.getDuckDBSettings();
        update(state => ({
          ...state,
          data: settings,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to load DuckDB settings';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async save(settings: { settings: Record<string, string>; db_path: string }) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.updateDuckDBSettings(settings);
        update(state => ({
          ...state,
          data: settings,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to save DuckDB settings';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    reset() {
      set(initialState);
    }
  };
}

export const duckdbStore = createDuckDBStore(); 
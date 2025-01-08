import { writable } from 'svelte/store';
import { api } from '../api';
import type { ServerConfig } from '../types';

export interface ServerState {
  data: ServerConfig | null;
  loading: boolean;
  error: string | null;
}

const initialState: ServerState = {
  data: null,
  loading: false,
  error: null
};

function createServerStore() {
  const { subscribe, set, update } = writable<ServerState>(initialState);

  return {
    subscribe,
    async load() {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        const config = await api.getServerConfig();
        update(state => ({
          ...state,
          data: config,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to load server configuration';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async save(config: ServerConfig) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.updateServerConfig(config);
        update(state => ({
          ...state,
          data: config,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to save server configuration';
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

export const serverStore = createServerStore(); 
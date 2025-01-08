import { writable } from 'svelte/store';
import { api } from '../api';
import type { ConnectionConfig } from '$lib/types';

export interface ConnectionsState {
  data: Record<string, ConnectionConfig> | null;
  loading: boolean;
  error: string | null;
}

const initialState: ConnectionsState = {
  data: null,
  loading: false,
  error: null
};

function createConnectionsStore() {
  const { subscribe, set, update } = writable<ConnectionsState>(initialState);

  return {
    subscribe,
    async load(name: string) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        const connection = await api.getConnection(name);
        update(state => ({
          ...state,
          data: { 
            ...(state.data || {}), 
            [name]: connection 
          },
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to load connection';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async save(name: string, config: ConnectionConfig) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.updateConnection(name, config);
        update(state => ({
          ...state,
          data: { 
            ...(state.data || {}), 
            [name]: config 
          },
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to save connection';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async testConnection(name: string) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.testConnection(name);
        update(state => ({ ...state, loading: false }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to test connection';
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

export const connectionsStore = createConnectionsStore(); 
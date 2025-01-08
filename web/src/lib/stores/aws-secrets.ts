import { writable } from 'svelte/store';
import { api } from '../api';
import type { AwsSecretsConfig } from '$lib/types';

export interface AwsSecretsState {
  data: AwsSecretsConfig | null;
  loading: boolean;
  error: string | null;
}

const initialState: AwsSecretsState = {
  data: null,
  loading: false,
  error: null
};

function createAwsSecretsStore() {
  const { subscribe, set, update } = writable<AwsSecretsState>(initialState);

  return {
    subscribe,
    async load() {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        const config = await api.getAwsSecretsConfig();
        update(state => ({
          ...state,
          data: config,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to load AWS Secrets configuration';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async save(config: AwsSecretsConfig) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.updateAwsSecretsConfig(config);
        update(state => ({
          ...state,
          data: config,
          loading: false
        }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to save AWS Secrets configuration';
        update(state => ({
          ...state,
          error: message,
          loading: false
        }));
        throw error;
      }
    },
    async testConnection(config: AwsSecretsConfig) {
      update(state => ({ ...state, loading: true, error: null }));
      try {
        await api.testAwsSecretsManagerConnection(config);
        update(state => ({ ...state, loading: false }));
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to test AWS Secrets connection';
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

export const awsSecretsStore = createAwsSecretsStore(); 
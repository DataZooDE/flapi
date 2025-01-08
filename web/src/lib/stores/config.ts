import { writable } from 'svelte/store';
import type { ProjectConfig } from '$lib/types';
import { api } from '$lib/api';

const DEFAULT_CONFIG: ProjectConfig = {
  project_name: '',
  version: '',
  server: {
    name: '',
    http_port: 8080,
    cache_schema: 'cache'
  },
  duckdb_settings: {}
};

function createConfigStore() {
  const { subscribe, set, update } = writable<ProjectConfig>(DEFAULT_CONFIG);

  return {
    subscribe,
    set,
    update,
    async load() {
      try {
        const config = await api.getProjectConfig();
        set(config);
      } catch (error) {
        console.error('Failed to load config:', error);
        throw error;
      }
    },
    async save() {
      try {
        let currentConfig: ProjectConfig | undefined;
        subscribe(value => {
          currentConfig = value;
        })();
        
        if (!currentConfig) {
          throw new Error('No config to save');
        }

        await api.updateProjectConfig(currentConfig);
      } catch (error) {
        console.error('Failed to save config:', error);
        throw error;
      }
    }
  };
}

export const configStore = createConfigStore(); 
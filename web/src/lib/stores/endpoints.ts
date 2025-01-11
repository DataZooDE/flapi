import { writable } from 'svelte/store';
import { api } from '$lib/api';
import type { EndpointConfig } from '$lib/types';

function createEndpointsStore() {
    const { subscribe, set, update } = writable<Record<string, EndpointConfig> | null>(null);

    return {
        subscribe,
        load: async (path: string) => {
            try {
                const config = await api.getEndpointConfig(path);
                 update(current => {
                    if (!current) {
                        return { [path]: config };
                    }
                    current[path] = config;
                    return current;
                });
            } catch (error) {
                console.error(`Error loading endpoint config for ${path}:`, error);
                set(null);
            }
        },
        loadAll: async () => {
            try {
                const projectConfig = await api.getProjectConfig();
                if (projectConfig && projectConfig.endpoints) {
                    set(projectConfig.endpoints);
                } else {
                    set({});
                }
            } catch (error) {
                console.error('Error loading all endpoints:', error);
                set(null);
            }
        },
        save: async (path: string, config: EndpointConfig) => {
             try {
                await api.updateEndpointConfig(path, config);
                update(current => {
                    if (!current) {
                        return { [path]: config };
                    }
                    current[path] = config;
                    return current;
                });
            } catch (error) {
                console.error(`Error saving endpoint config for ${path}:`, error);
                throw error;
            }
        },
        reset: () => set(null)
    };
}

export const endpointsStore = createEndpointsStore(); 
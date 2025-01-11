import { writable } from 'svelte/store';
import { api } from '$lib/api';
import type { ConnectionConfig } from '$lib/types';

function createConnectionsStore() {
    const { subscribe, set, update } = writable<Record<string, ConnectionConfig> | null>(null);

    return {
        subscribe,
        load: async (name: string) => {
            try {
                const config = await api.getConnection(name);
                update(current => {
                    if (!current) {
                        return { [name]: config };
                    }
                    current[name] = config;
                    return current;
                });
            } catch (error) {
                console.error(`Error loading connection config for ${name}:`, error);
                set(null);
            }
        },
        loadAll: async () => {
            try {
                const projectConfig = await api.getProjectConfig();
                if (projectConfig && projectConfig.connections) {
                    set(projectConfig.connections);
                } else {
                    set({});
                }
            } catch (error) {
                console.error('Error loading all connections:', error);
                set(null);
            }
        },
        save: async (name: string, config: ConnectionConfig) => {
            try {
                await api.updateConnection(name, config);
                update(current => {
                    if (!current) {
                        return { [name]: config };
                    }
                    current[name] = config;
                    return current;
                });
            } catch (error) {
                console.error(`Error saving connection config for ${name}:`, error);
                throw error;
            }
        },
        testConnection: async (name: string) => {
            try {
                await api.testConnection(name);
            } catch (error) {
                console.error(`Error testing connection ${name}:`, error);
                throw error;
            }
        },
        reset: () => set(null)
    };
}

export const connectionsStore = createConnectionsStore(); 
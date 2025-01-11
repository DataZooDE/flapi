import { writable } from 'svelte/store';
import { api } from '$lib/api';
import type { ProjectConfig } from '$lib/types';

function createProjectConfigStore() {
    const { subscribe, set, update } = writable<ProjectConfig | null>(null);

    return {
        subscribe,
        load: async () => {
            try {
                const config = await api.getProjectConfig();
                set(config);
            } catch (error) {
                console.error('Error loading project config:', error);
                set(null);
            }
        },
        save: async (config: ProjectConfig) => {
            try {
                await api.updateProjectConfig(config);
                set(config);
            } catch (error) {
                console.error('Error saving project config:', error);
                throw error;
            }
        },
        reset: () => set(null)
    };
}

export const projectConfigStore = createProjectConfigStore(); 
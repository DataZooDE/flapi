import { api } from '../api';
import { createLoadableStore, handleStoreError } from './store-factory';
import type { ProjectConfig } from '../types';

const store = createLoadableStore<ProjectConfig>();

/**
 * Store for managing project configuration
 */
export const projectStore = {
    ...store,
    
    /**
     * Loads project configuration from the API
     */
    async load() {
        store.setLoading(true);
        try {
            const data = await api.getProjectConfig();
            store.setData(data);
        } catch (error) {
            store.setError(handleStoreError(error));
        }
    },

    /**
     * Saves project configuration to the API
     */
    async save(config: ProjectConfig) {
        store.setLoading(true);
        try {
            await api.updateProjectConfig(config);
            store.setData(config);
        } catch (error) {
            store.setError(handleStoreError(error));
            throw error;
        }
    }
}; 
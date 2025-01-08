import { api } from '../api';
import { createLoadableStore, handleStoreError } from './store-factory';
import type { EndpointConfig } from '../types';
import { get } from 'svelte/store';

const store = createLoadableStore<Record<string, EndpointConfig>>();

/**
 * Store for managing endpoint configurations
 */
export const endpointsStore = {
    ...store,
    
    /**
     * Loads endpoint configuration from the API
     * @param path The endpoint path
     */
    async load(path: string) {
        store.setLoading(true);
        try {
            const data = await api.getEndpointConfig(path);
            store.setData({ [path]: data });
        } catch (error) {
            store.setError(handleStoreError(error));
        }
    },

    /**
     * Saves endpoint configuration to the API
     * @param path The endpoint path
     * @param config The endpoint configuration
     */
    async save(path: string, config: EndpointConfig) {
        store.setLoading(true);
        try {
            await api.updateEndpointConfig(path, config);
            store.setData({ [path]: config });
        } catch (error) {
            store.setError(handleStoreError(error));
            throw error;
        }
    },

    /**
     * Updates endpoint template
     * @param path The endpoint path
     * @param template The SQL template
     */
    async updateTemplate(path: string, template: string) {
        store.setLoading(true);
        try {
            await api.updateEndpointTemplate(path, template);
            const currentState = get(store);
            
            if (currentState.data && currentState.data[path]) {
                const updatedConfig = {
                    ...currentState.data[path],
                    'template-source': template
                };
                const newData = {
                    ...currentState.data,
                    [path]: updatedConfig
                };
                // Set loading to false before updating data
                store.setLoading(false);
                store.setData(newData);
            } else {
                // Set loading to false if no update was possible
                store.setLoading(false);
            }
        } catch (error) {
            store.setError(handleStoreError(error));
            throw error;
        }
    }
}; 
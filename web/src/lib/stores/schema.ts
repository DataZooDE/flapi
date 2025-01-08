import { api } from '../api';
import { createLoadableStore, handleStoreError } from './store-factory';
import type { SchemaInfo } from '../types';

const store = createLoadableStore<SchemaInfo>();

/**
 * Store for managing database schema information
 */
export const schemaStore = {
    ...store,
    
    /**
     * Loads schema information from the API
     */
    async load() {
        store.setLoading(true);
        try {
            const data = await api.getSchema();
            store.setData(data);
        } catch (error) {
            store.setError(handleStoreError(error));
        }
    }
}; 
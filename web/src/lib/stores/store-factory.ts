import { writable, type Writable } from 'svelte/store';
import type { StoreState } from '../types';
import { APIError } from '../api';
import { appState } from './app-state';

/**
 * Creates a loadable store with loading, error, and data states
 */
export function createLoadableStore<T>(): Writable<StoreState<T>> & {
    setLoading: (isLoading: boolean) => void;
    setError: (error: Error | null) => void;
    setData: (data: T | null) => void;
} {
    const store = writable<StoreState<T>>({ 
        isLoading: false, 
        error: null, 
        data: null 
    });
    
    return {
        ...store,
        setLoading: (isLoading: boolean) => {
            if (isLoading) {
                appState.startLoading();
            } else {
                appState.stopLoading();
            }
            store.update(state => ({ 
                ...state, 
                isLoading, 
                error: null,
                data: isLoading ? null : state.data 
            }));
        },
        setError: (error: Error | null) => {
            if (error) {
                appState.setError(error);
            } else {
                appState.clearError();
            }
            store.update(state => ({ 
                ...state, 
                error, 
                isLoading: false,
                data: error ? null : state.data 
            }));
        },
        setData: (data: T | null) => {
            store.update(state => ({ 
                ...state, 
                data, 
                isLoading: false, 
                error: null 
            }));
            appState.stopLoading();
            appState.clearError();
        }
    };
}

/**
 * Handles errors in store operations
 */
export function handleStoreError(error: unknown): Error {
    const processedError = error instanceof APIError ? error :
        error instanceof Error ? error :
        new Error('An unknown error occurred');
    
    appState.setError(processedError);
    return processedError;
} 
import { writable, derived } from 'svelte/store';
import type { LoadingState } from './types';

interface GlobalState {
    activeRequests: number;
    globalError: Error | null;
}

// Create the base store
const store = writable<GlobalState>({
    activeRequests: 0,
    globalError: null
});

// Create derived stores for loading and error states
export const isLoading = derived(store, $state => $state.activeRequests > 0);
export const globalError = derived(store, $state => $state.globalError);

// Create and export the app state store
export const appState = {
    subscribe: store.subscribe,
    
    startLoading: () => {
        store.update(state => ({
            ...state,
            activeRequests: state.activeRequests + 1
        }));
    },

    stopLoading: () => {
        store.update(state => ({
            ...state,
            activeRequests: Math.max(0, state.activeRequests - 1)
        }));
    },

    setError: (error: Error | null) => {
        store.update(state => ({
            ...state,
            globalError: error
        }));
    },

    clearError: () => {
        store.update(state => ({
            ...state,
            globalError: null
        }));
    }
}; 
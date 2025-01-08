import { writable, derived } from 'svelte/store';
import type { LoadingState } from './types';

interface GlobalState {
    activeRequests: number;
    globalError: Error | null;
}

function createAppStateStore() {
    const { subscribe, update, set } = writable<GlobalState>({
        activeRequests: 0,
        globalError: null
    });

    return {
        subscribe,
        
        /**
         * Increment active request counter
         */
        startLoading: () => {
            update(state => ({
                ...state,
                activeRequests: state.activeRequests + 1
            }));
        },

        /**
         * Decrement active request counter
         */
        stopLoading: () => {
            update(state => ({
                ...state,
                activeRequests: Math.max(0, state.activeRequests - 1)
            }));
        },

        /**
         * Set global error state
         */
        setError: (error: Error | null) => {
            update(state => ({
                ...state,
                globalError: error
            }));
        },

        /**
         * Clear global error state
         */
        clearError: () => {
            update(state => ({
                ...state,
                globalError: null
            }));
        }
    };
}

// Create the base store
export const appState = createAppStateStore();

// Derived store for loading state
export const isLoading = derived(
    appState,
    $state => $state.activeRequests > 0
);

// Derived store for error state
export const globalError = derived(
    appState,
    $state => $state.globalError
); 
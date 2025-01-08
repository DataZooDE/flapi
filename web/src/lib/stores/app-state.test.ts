import { describe, it, expect } from 'vitest';
import { appState, isLoading, globalError } from './app-state';
import { get } from 'svelte/store';

describe('appState', () => {
    it('should track loading state', () => {
        appState.startLoading();
        expect(get(isLoading)).toBe(true);

        appState.startLoading();
        expect(get(isLoading)).toBe(true);

        appState.stopLoading();
        expect(get(isLoading)).toBe(true);

        appState.stopLoading();
        expect(get(isLoading)).toBe(false);

        // Should not go below 0
        appState.stopLoading();
        expect(get(isLoading)).toBe(false);
    });

    it('should handle error state', () => {
        const error = new Error('Test error');
        
        appState.setError(error);
        expect(get(globalError)).toBe(error);

        appState.clearError();
        expect(get(globalError)).toBeNull();
    });
}); 
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { projectStore } from './project';
import { endpointsStore } from './endpoints';
import { connectionsStore } from './connections';
import { appState, isLoading, globalError } from './app-state';
import { get } from 'svelte/store';
import { api, APIError } from '../api';

vi.mock('../api', () => ({
    api: {
        getProjectConfig: vi.fn(),
        getEndpointConfig: vi.fn(),
        getConnection: vi.fn()
    },
    APIError: class APIError extends Error {
        constructor(message: string, public status: number, public details?: unknown) {
            super(message);
            this.name = 'APIError';
        }
    }
}));

describe('Store Interactions', () => {
    beforeEach(() => {
        vi.resetAllMocks();
        appState.clearError();
        // Reset loading state
        while (get(isLoading)) {
            appState.stopLoading();
        }
    });

    it('should handle concurrent loading states', async () => {
        // Mock successful responses with delays
        (api.getProjectConfig as any).mockImplementation(() => new Promise(resolve => 
            setTimeout(() => resolve({ project_name: 'test' }), 10)
        ));
        (api.getEndpointConfig as any).mockImplementation(() => new Promise(resolve => 
            setTimeout(() => resolve({ 'url-path': '/test' }), 10)
        ));
        (api.getConnection as any).mockImplementation(() => new Promise(resolve => 
            setTimeout(() => resolve({ properties: {} }), 10)
        ));

        // Start multiple loads
        const loadPromises = [
            projectStore.load(),
            endpointsStore.load('test'),
            connectionsStore.load('test')
        ];

        // Give time for loading states to be set
        await new Promise(resolve => setTimeout(resolve, 5));

        // Check loading state during requests
        expect(get(isLoading)).toBe(true);

        // Wait for all loads to complete
        await Promise.all(loadPromises);

        // Give time for loading states to be cleared
        await new Promise(resolve => setTimeout(resolve, 5));

        // Check loading state after completion
        expect(get(isLoading)).toBe(false);
    });

    it('should handle errors across stores', async () => {
        const error = new APIError('Test error', 500);
        (api.getProjectConfig as any).mockRejectedValue(error);

        // Attempt to load project
        await projectStore.load();

        // Check individual store error state
        const projectState = get(projectStore);
        expect(projectState.error).toBe(error);

        // Give time for error to propagate
        await new Promise(resolve => setTimeout(resolve, 5));

        // Check global error state
        expect(get(globalError)).toBe(error);
    });

    it('should maintain store independence', async () => {
        // Mock success for one store and error for another
        (api.getProjectConfig as any).mockResolvedValue({ project_name: 'test' });
        (api.getEndpointConfig as any).mockRejectedValue(new Error('Failed'));

        // Load both stores
        await projectStore.load();
        await endpointsStore.load('test');

        // Check states
        const projectState = get(projectStore);
        const endpointState = get(endpointsStore);

        expect(projectState.error).toBeNull();
        expect(projectState.data).toBeTruthy();
        expect(endpointState.error).toBeTruthy();
        expect(endpointState.data).toBeNull();
    });
}); 
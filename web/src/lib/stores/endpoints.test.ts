import { describe, it, expect, vi, beforeEach } from 'vitest';
import { endpointsStore } from './endpoints';
import { api, APIError } from '../api';
import { get } from 'svelte/store';

vi.mock('../api', () => ({
    api: {
        getEndpointConfig: vi.fn(),
        updateEndpointConfig: vi.fn(),
        updateEndpointTemplate: vi.fn()
    },
    APIError: class APIError extends Error {
        constructor(message: string, public status: number, public details?: unknown) {
            super(message);
            this.name = 'APIError';
        }
    }
}));

describe('endpointsStore', () => {
    const mockConfig = {
        'url-path': '/test',
        'template-source': 'SELECT * FROM test'
    };

    beforeEach(() => {
        vi.resetAllMocks();
    });

    it('should load endpoint configuration', async () => {
        (api.getEndpointConfig as any).mockResolvedValue(mockConfig);

        await endpointsStore.load('test');
        const state = get(endpointsStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        expect(state.data).toEqual({ test: mockConfig });
    });

    it('should handle load errors', async () => {
        const error = new APIError('Failed to load', 500);
        (api.getEndpointConfig as any).mockRejectedValue(error);

        await endpointsStore.load('test');
        const state = get(endpointsStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBe(error);
        expect(state.data).toBeNull();
    });

    it('should save endpoint configuration', async () => {
        await endpointsStore.save('test', mockConfig);
        const state = get(endpointsStore);

        expect(api.updateEndpointConfig).toHaveBeenCalledWith('test', mockConfig);
        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        expect(state.data).toEqual({ test: mockConfig });
    });

    it('should update endpoint template', async () => {
        const template = 'SELECT * FROM new_test';
        
        // First load the initial config
        (api.getEndpointConfig as any).mockResolvedValue(mockConfig);
        await endpointsStore.load('test');
        
        // Then update the template
        (api.updateEndpointTemplate as any).mockResolvedValue({});
        await endpointsStore.updateTemplate('test', template);
        
        const state = get(endpointsStore);

        expect(api.updateEndpointTemplate).toHaveBeenCalledWith('test', template);
        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        //expect(state.data?.test['template-source']).toBe(template);
    });
}); 
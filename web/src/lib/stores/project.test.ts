import { describe, it, expect, vi, beforeEach } from 'vitest';
import { projectStore } from './project';
import { api, APIError } from '../api';
import { get } from 'svelte/store';

vi.mock('../api', () => ({
    api: {
        getProjectConfig: vi.fn(),
        updateProjectConfig: vi.fn()
    },
    APIError: class APIError extends Error {
        constructor(message: string, public status: number, public details?: unknown) {
            super(message);
            this.name = 'APIError';
        }
    }
}));

describe('projectStore', () => {
    const mockConfig = {
        project_name: 'Test Project',
        project_description: 'Test Description',
        connections: {}
    };

    beforeEach(() => {
        vi.resetAllMocks();
    });

    it('should load project configuration', async () => {
        (api.getProjectConfig as any).mockResolvedValue(mockConfig);

        await projectStore.load();
        const state = get(projectStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        expect(state.data).toEqual(mockConfig);
    });

    it('should handle load errors', async () => {
        const error = new APIError('Failed to load', 500);
        (api.getProjectConfig as any).mockRejectedValue(error);

        await projectStore.load();
        const state = get(projectStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBe(error);
        expect(state.data).toBeNull();
    });

    it('should save project configuration', async () => {
        await projectStore.save(mockConfig);
        const state = get(projectStore);

        expect(api.updateProjectConfig).toHaveBeenCalledWith(mockConfig);
        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        expect(state.data).toEqual(mockConfig);
    });
}); 
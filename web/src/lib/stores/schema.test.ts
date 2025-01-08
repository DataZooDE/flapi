import { describe, it, expect, vi, beforeEach } from 'vitest';
import { schemaStore } from './schema';
import { api, APIError } from '../api';
import { get } from 'svelte/store';

vi.mock('../api', () => ({
    api: {
        getSchema: vi.fn()
    },
    APIError: class APIError extends Error {
        constructor(message: string, public status: number, public details?: unknown) {
            super(message);
            this.name = 'APIError';
        }
    }
}));

describe('schemaStore', () => {
    const mockSchema = {
        public: {
            tables: {
                users: {
                    is_view: false,
                    columns: {
                        id: {
                            type: 'INTEGER',
                            nullable: false
                        },
                        name: {
                            type: 'TEXT',
                            nullable: true
                        }
                    }
                }
            }
        }
    };

    beforeEach(() => {
        vi.resetAllMocks();
    });

    it('should load schema information', async () => {
        (api.getSchema as any).mockResolvedValue(mockSchema);

        await schemaStore.load();
        const state = get(schemaStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBeNull();
        expect(state.data).toEqual(mockSchema);
    });

    it('should handle load errors', async () => {
        const error = new APIError('Failed to load', 500);
        (api.getSchema as any).mockRejectedValue(error);

        await schemaStore.load();
        const state = get(schemaStore);

        expect(state.isLoading).toBe(false);
        expect(state.error).toBe(error);
        expect(state.data).toBeNull();
    });
}); 
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { APIClient, APIError } from './api';
import type { EndpointConfig, CacheConfig, AuthConfig, ProjectConfig, ServerConfig, DuckDBSettings, ConnectionConfig } from '$lib/types';

describe('APIClient', () => {
    let api: APIClient;
    let fetchMock: ReturnType<typeof vi.fn>;

    beforeEach(() => {
        fetchMock = vi.fn();
        global.fetch = fetchMock as unknown as typeof fetch;
        api = new APIClient();
    });

    describe('Endpoint Configuration', () => {
        it('should handle successful endpoint config request', async () => {
            const mockResponse: EndpointConfig = {
                'url-path': '/test',
                'template-source': 'test.sql',
                request: [
                    {
                        'field-name': 'param1',
                        'field-in': 'query',
                        description: 'Test parameter',
                        required: true,
                        type: 'string'
                    }
                ]
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockResponse),
            } as Response);

            const result = await api.getEndpointConfig('test');
            expect(result).toEqual(mockResponse);
        });

        it('should successfully update endpoint config', async () => {
            const config: EndpointConfig = {
                'url-path': '/test',
                'template-source': 'test.sql'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateEndpointConfig('test', config);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/endpoints/test',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(config)
                })
            );
        });
    });

    describe('Template Management', () => {
        it('should fetch endpoint template', async () => {
            const mockTemplate = {
                template: 'SELECT * FROM table WHERE id = {{id}}'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockTemplate),
            } as Response);

            const result = await api.getEndpointTemplate('test');
            expect(result).toEqual(mockTemplate);
        });

        it('should update endpoint template', async () => {
            const template = 'SELECT * FROM table WHERE id = {{id}}';

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateEndpointTemplate('test', template);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/endpoints/test/template',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify({ template })
                })
            );
        });
    });

    describe('Cache Management', () => {
        it('should fetch cache configuration', async () => {
            const mockCache: CacheConfig = {
                enabled: true,
                'refresh-time': '1h',
                'cache-table-name': 'cache_test',
                'cache-source': 'cache.sql'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockCache),
            } as Response);

            const result = await api.getEndpointCache('test');
            expect(result).toEqual(mockCache);
        });

        it('should update cache configuration', async () => {
            const cacheConfig: CacheConfig = {
                enabled: true,
                'refresh-time': '1h',
                'cache-table-name': 'cache_test',
                'cache-source': 'cache.sql'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateEndpointCache('test', cacheConfig);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/endpoints/test/cache',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(cacheConfig)
                })
            );
        });
    });

    describe('Schema Management', () => {
        it('should fetch schema information', async () => {
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
                                    type: 'VARCHAR',
                                    nullable: true
                                }
                            }
                        }
                    }
                }
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockSchema),
            } as Response);

            const result = await api.getSchema();
            expect(result).toEqual(mockSchema);
        });
    });

    describe('Error Handling', () => {
        it('should handle API errors with JSON response', async () => {
            const errorResponse = { message: 'Not found' };
            
            (fetchMock as any).mockResolvedValueOnce({
                ok: false,
                status: 404,
                statusText: 'Not Found',
                json: () => Promise.resolve(errorResponse),
            } as Response);

            await expect(api.getEndpointConfig('nonexistent')).rejects.toThrow(
                APIError
            );
        });

        it('should handle API errors with text response', async () => {
            (fetchMock as any).mockResolvedValueOnce({
                ok: false,
                status: 500,
                statusText: 'Internal Server Error',
                json: () => Promise.reject(new Error('Invalid JSON')),
                text: () => Promise.resolve('Server Error'),
            } as Response);

            await expect(api.getEndpointConfig('test')).rejects.toThrow(
                APIError
            );
        });
    });

    describe('Request Headers', () => {
        it('should send correct content-type header', async () => {
            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            const [, options] = fetchMock.mock.calls[0];
            expect(options.headers).toEqual(
                expect.objectContaining({
                    'Content-Type': 'application/json'
                })
            );
        });
    });

    describe('Interceptors', () => {
        it('should apply request interceptors', async () => {
            const interceptor = vi.fn((config: RequestInit) => ({
                ...config,
                headers: {
                    ...config.headers,
                    'X-Test': 'test-value'
                }
            }));

            const removeInterceptor = api.addRequestInterceptor(interceptor);

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            expect(interceptor).toHaveBeenCalled();
            expect(fetchMock).toHaveBeenCalledWith(
                expect.any(String),
                expect.objectContaining({
                    headers: expect.objectContaining({
                        'X-Test': 'test-value'
                    })
                })
            );

            removeInterceptor();
        });

        it('should apply response interceptors', async () => {
            const interceptor = vi.fn((response: Response) => response);

            const removeInterceptor = api.addResponseInterceptor(interceptor);

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            expect(interceptor).toHaveBeenCalled();

            removeInterceptor();
        });

        it('should remove interceptors when using the cleanup function', async () => {
            const interceptor = vi.fn((config: RequestInit) => config);
            const removeInterceptor = api.addRequestInterceptor(interceptor);

            removeInterceptor();

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            expect(interceptor).not.toHaveBeenCalled();
        });
    });

    describe('Authentication', () => {
        it('should add basic auth headers when configured', async () => {
            api.setBasicAuth('testuser', 'testpass');

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            const [, options] = fetchMock.mock.calls[0];
            expect(options.headers).toEqual(
                expect.objectContaining({
                    'Authorization': 'Basic ' + btoa('testuser:testpass')
                })
            );
        });

        it('should add bearer token when configured', async () => {
            api.setBearerAuth('test-token');

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            const [, options] = fetchMock.mock.calls[0];
            expect(options.headers).toEqual(
                expect.objectContaining({
                    'Authorization': 'Bearer test-token'
                })
            );
        });

        it('should clear auth headers when clearAuth is called', async () => {
            api.setBearerAuth('test-token');
            api.clearAuth();

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.getEndpointConfig('test');

            const [, options] = fetchMock.mock.calls[0];
            expect(options.headers).not.toHaveProperty('Authorization');
        });

        it('should handle auth configuration endpoints', async () => {
            const mockAuthConfig: AuthConfig = {
                enabled: true,
                type: 'basic',
                users: [{ username: 'test', password: 'pass' }]
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockAuthConfig),
            } as Response);

            const result = await api.getAuthConfig('test');
            expect(result).toEqual(mockAuthConfig);
        });

        it('should test AWS Secrets Manager connection', async () => {
            const config = {
                secret_name: 'test-secret',
                region: 'us-east-1',
                secret_table: 'auth_users'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.testAwsSecretsManagerConnection(config);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/test/aws-secrets-manager',
                expect.objectContaining({
                    method: 'POST',
                    body: JSON.stringify(config)
                })
            );
        });
    });

    describe('Project Configuration', () => {
        it('should fetch project configuration', async () => {
            const mockConfig: ProjectConfig = {
                project_name: 'Test Project',
                project_description: 'Test Description',
                connections: {},
                server: {
                    server_name: 'test-server',
                    http_port: 8080,
                    cache_schema: 'cache'
                },
                duckdb: {
                    settings: {
                        'memory_limit': '4GB'
                    },
                    db_path: ':memory:'
                }
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockConfig),
            } as Response);

            const result = await api.getProjectConfig();
            expect(result).toEqual(mockConfig);
        });

        it('should update project configuration', async () => {
            const config: ProjectConfig = {
                project_name: 'Test Project',
                project_description: 'Test Description',
                connections: {}
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateProjectConfig(config);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/project',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(config)
                })
            );
        });
    });

    describe('Server Configuration', () => {
        it('should fetch server configuration', async () => {
            const mockConfig: ServerConfig = {
                server_name: 'test-server',
                http_port: 8080,
                cache_schema: 'cache'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockConfig),
            } as Response);

            const result = await api.getServerConfig();
            expect(result).toEqual(mockConfig);
        });

        it('should update server configuration', async () => {
            const config: ServerConfig = {
                server_name: 'test-server',
                http_port: 8080
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateServerConfig(config);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/server',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(config)
                })
            );
        });
    });

    describe('DuckDB Settings', () => {
        it('should fetch DuckDB settings', async () => {
            const mockSettings: DuckDBSettings = {
                settings: {
                    'memory_limit': '4GB',
                    'threads': '4'
                },
                db_path: ':memory:'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockSettings),
            } as Response);

            const result = await api.getDuckDBSettings();
            expect(result).toEqual(mockSettings);
        });

        it('should update DuckDB settings', async () => {
            const settings: DuckDBSettings = {
                settings: {
                    'memory_limit': '4GB'
                }
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateDuckDBSettings(settings);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/duckdb',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(settings)
                })
            );
        });
    });

    describe('Connection Configuration', () => {
        it('should handle connection with logging config', async () => {
            const mockConfig: ConnectionConfig = {
                init: 'SELECT 1',
                properties: {
                    host: 'localhost',
                    port: 5432
                },
                'log-queries': true,
                'log-parameters': true,
                allow: '*'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(mockConfig),
            } as Response);

            const result = await api.getConnection('test');
            expect(result).toEqual(mockConfig);
        });

        it('should update connection with logging config', async () => {
            const config: ConnectionConfig = {
                init: 'SELECT 1',
                properties: {
                    host: 'localhost',
                    port: 5432
                },
                'log-queries': true,
                'log-parameters': true,
                allow: '*'
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve({}),
            } as Response);

            await api.updateConnection('test', config);
            expect(fetchMock).toHaveBeenCalledWith(
                '/api/config/connections/test',
                expect.objectContaining({
                    method: 'PUT',
                    body: JSON.stringify(config)
                })
            );
        });

        it('should handle connection without optional fields', async () => {
            const config: ConnectionConfig = {
                properties: {
                    host: 'localhost',
                    port: 5432
                }
            };

            (fetchMock as any).mockResolvedValueOnce({
                ok: true,
                json: () => Promise.resolve(config),
            } as Response);

            const result = await api.getConnection('test');
            expect(result).toEqual(config);
        });
    });
}); 
// API-specific types
export interface RequestParameter {
    'field-name': string;
    'field-in': 'query' | 'path' | 'header' | 'body';
    description?: string;
    required?: boolean;
    type?: string;
    format?: string;
}

export interface EndpointConfig {
    'url-path': string;
    'template-source': string;
    request?: RequestParameter[];
}

export interface CacheConfig {
    enabled: boolean;
    'refresh-time': string;
    'cache-table-name': string;
    'cache-source': string;
}

export interface SchemaInfo {
    [schema_name: string]: {
        tables: {
            [table_name: string]: {
                is_view: boolean;
                columns: {
                    [column_name: string]: {
                        type: string;
                        nullable: boolean;
                    };
                };
            };
        };
    };
}

export type AuthType = 'basic' | 'bearer' | 'aws-secretsmanager';

export interface AuthUser {
    username: string;
    password: string;
}

export interface AwsSecretsManagerConfig {
    secret_name: string;
    region: string;
    secret_id?: string;
    secret_key?: string;
    secret_table: string;
    init?: string;
}

export interface AuthConfig {
    enabled: boolean;
    type: AuthType;
    users?: AuthUser[];
    from_aws_secretmanager?: AwsSecretsManagerConfig;
}

export interface ServerConfig {
    server_name?: string;
    http_port?: number;
    cache_schema?: string;
}

export interface DuckDBSettings {
    settings?: Record<string, string>;
    db_path?: string;
}

export interface ProjectConfig {
    project_name: string;
    project_description: string;
    server?: ServerConfig;
    duckdb?: DuckDBSettings;
    connections: Record<string, ConnectionConfig>;
}

export interface ConnectionConfig {
    init?: string;
    properties: Record<string, unknown>;
    'log-queries'?: boolean;
    'log-parameters'?: boolean;
    allow?: string;
} 
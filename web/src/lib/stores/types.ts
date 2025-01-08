import type { 
    ProjectConfig, 
    EndpointConfig, 
    ConnectionConfig, 
    SchemaInfo,
    ServerConfig,
    DuckDBSettings
} from '../types';

export interface LoadingState {
    isLoading: boolean;
    error: Error | null;
}

export interface StoreState<T> extends LoadingState {
    data: T | null;
}

// Export specific store state types
export type ProjectState = StoreState<ProjectConfig>;
export type EndpointsState = StoreState<Record<string, EndpointConfig>>;
export type ConnectionsState = StoreState<Record<string, ConnectionConfig>>;
export type SchemaState = StoreState<SchemaInfo>;
export type ServerState = StoreState<ServerConfig>;
export type DuckDBState = StoreState<DuckDBSettings>; 
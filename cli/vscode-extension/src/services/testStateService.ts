import * as vscode from 'vscode';

/**
 * Unified test state for REST endpoints and SQL templates
 */
export interface TestState {
  /** Unique identifier (slug for REST, file path for SQL) */
  identifier: string;
  /** Type of test */
  type: 'rest' | 'sql';
  /** Base URL for REST endpoints */
  baseUrl?: string;
  /** HTTP method for REST endpoints */
  method?: string;
  /** Template file path for SQL templates */
  templatePath?: string;
  /** Associated YAML config path */
  yamlConfigPath?: string;
  /** Last time this state was accessed */
  lastUsed: Date;
  /** Request parameters (Mustache variables) */
  parameters: Record<string, string>;
  /** HTTP headers (REST only) */
  headers: Record<string, string>;
  /** Request body (REST POST/PUT only) */
  body?: string;
  /** Authentication configuration (REST only) */
  authConfig?: AuthConfig;
  /** History of previous requests */
  history: TestHistoryEntry[];
}

export interface AuthConfig {
  type: 'basic' | 'bearer' | 'apikey' | 'none';
  username?: string;
  password?: string;
  token?: string;
  apiKey?: string;
  headerName?: string;
}

export interface TestHistoryEntry {
  timestamp: Date;
  parameters: Record<string, string>;
  headers?: Record<string, string>;
  body?: string;
  response: TestResponse;
}

export interface TestResponse {
  status?: number;
  statusText?: string;
  time: number; // milliseconds
  size: number; // bytes
  headers?: Record<string, string>;
  body: string;
  contentType?: string;
  error?: string;
}

/**
 * Service for persisting test states across REST endpoints and SQL templates
 * 
 * This service provides unified storage for:
 * - REST endpoint parameters, headers, and history
 * - SQL template Mustache variables and execution history
 */
export class TestStateService {
  private static readonly STORAGE_KEY_PREFIX = 'flapi.test.';
  private static readonly WORKSPACE_DEFAULTS_KEY = 'flapi.workspace.defaults';
  private static readonly MAX_HISTORY_ITEMS = 10;

  constructor(private context: vscode.ExtensionContext) {}

  /**
   * Get stored test state by identifier
   */
  getState(identifier: string): TestState | undefined {
    const key = this.getStorageKey(identifier);
    const stored = this.context.workspaceState.get<TestState>(key);
    
    if (stored) {
      // Convert date strings back to Date objects
      stored.lastUsed = new Date(stored.lastUsed);
      stored.history = stored.history.map(h => ({
        ...h,
        timestamp: new Date(h.timestamp)
      }));
    }
    
    return stored;
  }

  /**
   * Save test state
   */
  async saveState(state: TestState): Promise<void> {
    const key = this.getStorageKey(state.identifier);
    await this.context.workspaceState.update(key, state);
  }

  /**
   * Update parameters
   */
  async updateParameters(identifier: string, parameters: Record<string, string>): Promise<void> {
    const state = this.getState(identifier);
    if (state) {
      state.parameters = parameters;
      state.lastUsed = new Date();
      await this.saveState(state);
    }
  }

  /**
   * Update headers (REST only)
   */
  async updateHeaders(identifier: string, headers: Record<string, string>): Promise<void> {
    const state = this.getState(identifier);
    if (state && state.type === 'rest') {
      state.headers = headers;
      state.lastUsed = new Date();
      await this.saveState(state);
    }
  }

  /**
   * Update body (REST POST/PUT only)
   */
  async updateBody(identifier: string, body: string): Promise<void> {
    const state = this.getState(identifier);
    if (state && state.type === 'rest') {
      state.body = body;
      state.lastUsed = new Date();
      await this.saveState(state);
    }
  }

  /**
   * Update authentication config (REST only)
   */
  async updateAuthConfig(identifier: string, authConfig: AuthConfig): Promise<void> {
    const state = this.getState(identifier);
    if (state && state.type === 'rest') {
      state.authConfig = authConfig;
      state.lastUsed = new Date();
      await this.saveState(state);
    }
  }

  /**
   * Add an entry to history
   */
  async addToHistory(identifier: string, historyItem: TestHistoryEntry): Promise<void> {
    const state = this.getState(identifier);
    if (state) {
      state.history.unshift(historyItem);
      
      // Keep only the last N items
      if (state.history.length > TestStateService.MAX_HISTORY_ITEMS) {
        state.history = state.history.slice(0, TestStateService.MAX_HISTORY_ITEMS);
      }
      
      await this.saveState(state);
    }
  }

  /**
   * Clear history
   */
  async clearHistory(identifier: string): Promise<void> {
    const state = this.getState(identifier);
    if (state) {
      state.history = [];
      await this.saveState(state);
    }
  }

  /**
   * Get workspace-level default headers
   */
  getWorkspaceDefaults(): Record<string, string> {
    return this.context.workspaceState.get<Record<string, string>>(
      TestStateService.WORKSPACE_DEFAULTS_KEY,
      {}
    );
  }

  /**
   * Set workspace-level default headers
   */
  async setWorkspaceDefaults(defaults: Record<string, string>): Promise<void> {
    await this.context.workspaceState.update(
      TestStateService.WORKSPACE_DEFAULTS_KEY,
      defaults
    );
  }

  /**
   * Initialize state for a REST endpoint
   */
  async initializeRestState(
    slug: string,
    baseUrl: string,
    method: string,
    yamlConfigPath?: string,
    initialParams?: Record<string, string>,
    initialHeaders?: Record<string, string>
  ): Promise<TestState> {
    const existing = this.getState(slug);
    
    if (existing && existing.type === 'rest') {
      // Update in case they changed
      existing.method = method;
      existing.baseUrl = baseUrl;
      existing.yamlConfigPath = yamlConfigPath;
      return existing;
    }

    const workspaceDefaults = this.getWorkspaceDefaults();
    
    const newState: TestState = {
      identifier: slug,
      type: 'rest',
      baseUrl,
      method,
      yamlConfigPath,
      lastUsed: new Date(),
      parameters: initialParams || {},
      headers: { ...workspaceDefaults, ...(initialHeaders || {}) },
      history: []
    };

    await this.saveState(newState);
    return newState;
  }

  /**
   * Initialize state for a SQL template
   */
  async initializeSqlState(
    templatePath: string,
    yamlConfigPath: string,
    initialParams?: Record<string, string>
  ): Promise<TestState> {
    const existing = this.getState(templatePath);
    
    if (existing && existing.type === 'sql') {
      // Update YAML config path in case it changed
      existing.yamlConfigPath = yamlConfigPath;
      return existing;
    }

    const newState: TestState = {
      identifier: templatePath,
      type: 'sql',
      templatePath,
      yamlConfigPath,
      lastUsed: new Date(),
      parameters: initialParams || {},
      headers: {},
      history: []
    };

    await this.saveState(newState);
    return newState;
  }

  /**
   * Get all stored test states (for debugging/management)
   */
  getAllStates(): Map<string, TestState> {
    const states = new Map<string, TestState>();
    const keys = this.context.workspaceState.keys();
    
    for (const key of keys) {
      if (key.startsWith(TestStateService.STORAGE_KEY_PREFIX)) {
        const identifier = key.substring(TestStateService.STORAGE_KEY_PREFIX.length);
        const state = this.getState(identifier);
        if (state) {
          states.set(identifier, state);
        }
      }
    }
    
    return states;
  }

  /**
   * Get all REST endpoint states
   */
  getRestStates(): Map<string, TestState> {
    const allStates = this.getAllStates();
    const restStates = new Map<string, TestState>();
    
    for (const [key, state] of allStates) {
      if (state.type === 'rest') {
        restStates.set(key, state);
      }
    }
    
    return restStates;
  }

  /**
   * Get all SQL template states
   */
  getSqlStates(): Map<string, TestState> {
    const allStates = this.getAllStates();
    const sqlStates = new Map<string, TestState>();
    
    for (const [key, state] of allStates) {
      if (state.type === 'sql') {
        sqlStates.set(key, state);
      }
    }
    
    return sqlStates;
  }

  /**
   * Clear all stored data
   */
  async clearAll(): Promise<void> {
    const keys = this.context.workspaceState.keys();
    for (const key of keys) {
      if (key.startsWith(TestStateService.STORAGE_KEY_PREFIX) || 
          key === TestStateService.WORKSPACE_DEFAULTS_KEY) {
        await this.context.workspaceState.update(key, undefined);
      }
    }
  }

  /**
   * Delete a specific test state
   */
  async deleteState(identifier: string): Promise<void> {
    const key = this.getStorageKey(identifier);
    await this.context.workspaceState.update(key, undefined);
  }

  private getStorageKey(identifier: string): string {
    // Normalize identifier for storage key
    const normalized = identifier.replace(/[^a-zA-Z0-9_\-./]/g, '_');
    return `${TestStateService.STORAGE_KEY_PREFIX}${normalized}`;
  }
}


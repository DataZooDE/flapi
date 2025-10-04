import * as vscode from 'vscode';
import { EndpointTestState, RequestHistory } from '../types/endpointTest';

/**
 * Service for persisting endpoint test parameters and history
 */
export class ParameterStorageService {
  private static readonly STORAGE_KEY_PREFIX = 'flapi.endpoint.';
  private static readonly WORKSPACE_DEFAULTS_KEY = 'flapi.workspace.defaults';
  private static readonly MAX_HISTORY_ITEMS = 10;

  constructor(private context: vscode.ExtensionContext) {}

  /**
   * Get stored test state for an endpoint
   */
  getEndpointState(slug: string): EndpointTestState | undefined {
    const key = this.getStorageKey(slug);
    const stored = this.context.workspaceState.get<EndpointTestState>(key);
    
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
   * Save test state for an endpoint
   */
  async saveEndpointState(state: EndpointTestState): Promise<void> {
    const key = this.getStorageKey(state.slug);
    await this.context.workspaceState.update(key, state);
  }

  /**
   * Update parameters for an endpoint
   */
  async updateParameters(slug: string, parameters: Record<string, string>): Promise<void> {
    const state = this.getEndpointState(slug);
    if (state) {
      state.parameters = parameters;
      state.lastUsed = new Date();
      await this.saveEndpointState(state);
    }
  }

  /**
   * Update headers for an endpoint
   */
  async updateHeaders(slug: string, headers: Record<string, string>): Promise<void> {
    const state = this.getEndpointState(slug);
    if (state) {
      state.headers = headers;
      state.lastUsed = new Date();
      await this.saveEndpointState(state);
    }
  }

  /**
   * Add a request to history
   */
  async addToHistory(slug: string, historyItem: RequestHistory): Promise<void> {
    const state = this.getEndpointState(slug);
    if (state) {
      state.history.unshift(historyItem);
      
      // Keep only the last N items
      if (state.history.length > ParameterStorageService.MAX_HISTORY_ITEMS) {
        state.history = state.history.slice(0, ParameterStorageService.MAX_HISTORY_ITEMS);
      }
      
      await this.saveEndpointState(state);
    }
  }

  /**
   * Clear history for an endpoint
   */
  async clearHistory(slug: string): Promise<void> {
    const state = this.getEndpointState(slug);
    if (state) {
      state.history = [];
      await this.saveEndpointState(state);
    }
  }

  /**
   * Get workspace-level default headers
   */
  getWorkspaceDefaults(): Record<string, string> {
    return this.context.workspaceState.get<Record<string, string>>(
      ParameterStorageService.WORKSPACE_DEFAULTS_KEY,
      {}
    );
  }

  /**
   * Set workspace-level default headers
   */
  async setWorkspaceDefaults(defaults: Record<string, string>): Promise<void> {
    await this.context.workspaceState.update(
      ParameterStorageService.WORKSPACE_DEFAULTS_KEY,
      defaults
    );
  }

  /**
   * Initialize state for a new endpoint
   */
  async initializeEndpointState(
    slug: string,
    baseUrl: string,
    method: string,
    initialParams?: Record<string, string>
  ): Promise<EndpointTestState> {
    const existing = this.getEndpointState(slug);
    
    if (existing) {
      // Update method and baseUrl in case they changed
      existing.method = method;
      existing.baseUrl = baseUrl;
      return existing;
    }

    const workspaceDefaults = this.getWorkspaceDefaults();
    
    const newState: EndpointTestState = {
      slug,
      baseUrl,
      method,
      lastUsed: new Date(),
      parameters: initialParams || {},
      headers: { ...workspaceDefaults },
      history: []
    };

    await this.saveEndpointState(newState);
    return newState;
  }

  /**
   * Get all stored endpoint states (for debugging/management)
   */
  getAllEndpointStates(): Map<string, EndpointTestState> {
    const states = new Map<string, EndpointTestState>();
    const keys = this.context.workspaceState.keys();
    
    for (const key of keys) {
      if (key.startsWith(ParameterStorageService.STORAGE_KEY_PREFIX)) {
        const slug = key.substring(ParameterStorageService.STORAGE_KEY_PREFIX.length);
        const state = this.getEndpointState(slug);
        if (state) {
          states.set(slug, state);
        }
      }
    }
    
    return states;
  }

  /**
   * Clear all stored data
   */
  async clearAll(): Promise<void> {
    const keys = this.context.workspaceState.keys();
    for (const key of keys) {
      if (key.startsWith(ParameterStorageService.STORAGE_KEY_PREFIX) || 
          key === ParameterStorageService.WORKSPACE_DEFAULTS_KEY) {
        await this.context.workspaceState.update(key, undefined);
      }
    }
  }

  private getStorageKey(slug: string): string {
    // Normalize slug for storage key
    const normalized = slug.replace(/[^a-zA-Z0-9_-]/g, '_');
    return `${ParameterStorageService.STORAGE_KEY_PREFIX}${normalized}`;
  }
}


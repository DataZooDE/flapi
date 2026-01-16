import * as fs from 'node:fs/promises';
import * as path from 'node:path';
import * as os from 'node:os';
import { google } from 'google-auth-library';
import { Console } from '../console';
import { GeminiAuthConfig } from '../types';

const CONFIG_DIR = path.join(os.homedir(), '.flapi');
const CONFIG_FILE = path.join(CONFIG_DIR, 'config.json');

interface StoredConfig {
  gemini?: GeminiAuthConfig;
}

export class GeminiAuthManager {
  async getAuthConfig(): Promise<GeminiAuthConfig | null> {
    try {
      // Check environment variable first
      const envKey = process.env.FLAPI_GEMINI_KEY;
      if (envKey) {
        return {
          auth_method: 'api_key',
          api_key: envKey,
          model: 'gemini-2.0-flash-exp',
          max_tokens: 8192,
        };
      }

      // Check stored config file
      const config = await this.loadConfig();
      return config?.gemini || null;
    } catch (error) {
      return null;
    }
  }

  async storeApiKey(apiKey: string): Promise<void> {
    const config = await this.loadConfig();
    if (!config.gemini) {
      config.gemini = {};
    }
    config.gemini.auth_method = 'api_key';
    config.gemini.api_key = apiKey;
    config.gemini.model = 'gemini-2.0-flash-exp';
    config.gemini.max_tokens = 8192;
    await this.saveConfig(config);
  }

  async storeOAuthToken(
    accessToken: string,
    refreshToken?: string,
    expiry?: number
  ): Promise<void> {
    const config = await this.loadConfig();
    if (!config.gemini) {
      config.gemini = {};
    }
    config.gemini.auth_method = 'oauth';
    config.gemini.oauth = {
      access_token: accessToken,
      refresh_token: refreshToken,
      expiry,
    };
    config.gemini.model = 'gemini-2.0-flash-exp';
    config.gemini.max_tokens = 8192;
    await this.saveConfig(config);
  }

  async hasStoredAuth(): Promise<boolean> {
    const config = await this.getAuthConfig();
    return config !== null;
  }

  private async loadConfig(): Promise<StoredConfig> {
    try {
      const content = await fs.readFile(CONFIG_FILE, 'utf-8');
      return JSON.parse(content);
    } catch {
      return {};
    }
  }

  private async saveConfig(config: StoredConfig): Promise<void> {
    try {
      await fs.mkdir(CONFIG_DIR, { recursive: true });
      // Write with restricted permissions (user read/write only)
      await fs.writeFile(CONFIG_FILE, JSON.stringify(config, null, 2), {
        mode: 0o600,
      });
    } catch (error) {
      throw new Error(
        `Failed to save Gemini configuration: ${error instanceof Error ? error.message : String(error)}`
      );
    }
  }

  async clearAuth(): Promise<void> {
    const config = await this.loadConfig();
    delete config.gemini;
    await this.saveConfig(config);
  }
}

export const authManager = new GeminiAuthManager();

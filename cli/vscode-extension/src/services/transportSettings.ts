import * as vscode from 'vscode';

/**
 * The one place the extension reads its HTTP settings (`flapi.serverUrl`,
 * `flapi.timeout`, `flapi.retries`, `flapi.insecure`). Every client - the shared
 * ConfigService transport and the REST endpoint tester - is built from this, so
 * token, TLS, timeout and retry behaviour cannot drift apart.
 */
export interface TransportSettings {
  serverUrl: string;
  /** Seconds. */
  timeout: number;
  retries: number;
  verifyTls: boolean;
}

export function readTransportSettings(): TransportSettings {
  const config = vscode.workspace.getConfiguration('flapi');
  return {
    serverUrl: config.get<string>('serverUrl', 'http://localhost:8080'),
    timeout: config.get<number>('timeout', 30),
    retries: config.get<number>('retries', 3),
    verifyTls: !config.get<boolean>('insecure', false),
  };
}

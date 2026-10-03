/**
 * A minimal stand-in for the `vscode` module, which only exists inside the VS Code
 * extension host. It models the host's storage objects faithfully (Memento,
 * SecretStorage); it never stands in for flAPI.
 */
export class FakeMemento {
  private data = new Map<string, unknown>();
  get<T>(key: string, fallback?: T): T | undefined {
    return (this.data.has(key) ? (this.data.get(key) as T) : fallback);
  }
  async update(key: string, value: unknown): Promise<void> {
    if (value === undefined) this.data.delete(key);
    else this.data.set(key, value);
  }
  keys(): readonly string[] {
    return [...this.data.keys()];
  }
  /** Everything stored, serialised: what would land in the workspace's storage file. */
  dump(): string {
    return JSON.stringify([...this.data.entries()]);
  }
}

export class FakeSecretStorage {
  private data = new Map<string, string>();
  async get(key: string): Promise<string | undefined> {
    return this.data.get(key);
  }
  async store(key: string, value: string): Promise<void> {
    this.data.set(key, value);
  }
  async delete(key: string): Promise<void> {
    this.data.delete(key);
  }
}

export function fakeContext() {
  return {
    workspaceState: new FakeMemento(),
    globalState: new FakeMemento(),
    secrets: new FakeSecretStorage(),
    subscriptions: [] as { dispose(): void }[],
  };
}

// Surface referenced at import time by extension modules.
export const ViewColumn = { One: 1, Two: 2 };

/** The host side of a webview panel; the test wires it to a jsdom page. */
export class FakeWebviewPanel {
  title = '';
  webview: {
    html: string;
    cspSource: string;
    postMessage: (m: unknown) => Promise<boolean>;
    onDidReceiveMessage: (h: (m: any) => unknown) => { dispose(): void };
    asWebviewUri: (u: unknown) => unknown;
  };
  /** Deliver a message to the extension (what the page's postMessage does). */
  receive: ((m: unknown) => unknown) | undefined;
  /** Set by the test: deliver a message to the page. */
  toPage: (m: unknown) => void = () => undefined;
  constructor() {
    const self = this;
    this.webview = {
      html: '',
      cspSource: 'vscode-resource:',
      postMessage: async (m) => { self.toPage(m); return true; },
      onDidReceiveMessage: (h) => { self.receive = h; return { dispose() {} }; },
      asWebviewUri: (u) => u,
    };
  }
  reveal() {}
  dispose() {}
  onDidDispose() { return { dispose() {} }; }
}
export const panels: FakeWebviewPanel[] = [];
export const window = {
  showInformationMessage: () => undefined,
  showErrorMessage: () => undefined,
  createWebviewPanel: () => { const p = new FakeWebviewPanel(); panels.push(p); return p; },
};
export const workspace = { getConfiguration: () => ({ get: (_: string, d: unknown) => d }) };
export class EventEmitter<T> {
  private handlers: ((e: T) => void)[] = [];
  event = (h: (e: T) => void) => { this.handlers.push(h); return { dispose() {} }; };
  fire(e: T) { this.handlers.forEach((h) => h(e)); }
}
export class TreeItem { constructor(public label?: string, public collapsibleState?: number) {} }
export const TreeItemCollapsibleState = { None: 0, Collapsed: 1, Expanded: 2 };
export class ThemeIcon { constructor(public id: string) {} }
export const Uri = { joinPath: (_b: unknown, ...p: string[]) => ({ fsPath: p.join('/') }), file: (p: string) => ({ fsPath: p, toString: () => `file://${p}` }), parse: (s: string) => ({ toString: () => s }) };

// --- extras used by the validator ---
export const workspaceFolders = [{ uri: { fsPath: process.env.FLAPI_IT_WORKDIR ?? '/work' } }];
(workspace as any).workspaceFolders = workspaceFolders;
(workspace as any).asRelativePath = (p: string) => p.replace((process.env.FLAPI_IT_WORKDIR ?? '') + '/', '');
export const languages = { createDiagnosticCollection: () => ({ set() {}, delete() {}, dispose() {} }) };
export const DiagnosticSeverity = { Error: 0, Warning: 1 };
(window as any).showWarningMessage = () => undefined;

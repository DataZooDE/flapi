import type { EditorStatePayload } from './lib/types';

type MessagePayload =
  | { type: 'load' }
  | { type: 'refresh' }
  | { type: 'save-request'; payload: { config: any } }
  | { type: 'save-template'; payload: { template: string } }
  | { type: 'save-cache'; payload: { cache: any; cacheTemplate: string } }
  | { type: 'expand-template'; payload: { parameters: any } }
  | { type: 'test-template'; payload: { parameters: any } }
  | { type: 'auto-expand-toggle'; payload: { value: boolean; section: 'sql' | 'cache' } }
  | { type: 'request-schema' };

type IncomingPayload =
  | { type: 'load-result'; payload: EditorStatePayload }
  | { type: 'schema-load'; payload: any }
  | { type: 'save-template-success' }
  | { type: 'save-cache-success' }
  | { type: 'save-request-success' }
  | { type: 'expand-template-success'; payload: any }
  | { type: 'test-template-success'; payload: any }
  | { type: 'error'; payload: { message: string } };

export class MessageBus {
  private readonly vscode = acquireVsCodeApi();
  private listeners: Array<(message: IncomingPayload) => void> = [];

  postMessage(message: MessagePayload) {
    this.vscode.postMessage(message);
  }

  onMessage(listener: (message: IncomingPayload) => void) {
    this.listeners.push(listener);
  }

  dispatch(event: MessageEvent) {
    for (const listener of this.listeners) {
      listener(event.data);
    }
  }

  persistState(state: unknown) {
    this.vscode.setState(state);
  }

  restoreState<T>(defaultState: T): T {
    const restored = this.vscode.getState();
    return restored ? { ...defaultState, ...restored } : defaultState;
  }
}

export const bus = new MessageBus();

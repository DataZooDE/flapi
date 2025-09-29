declare function acquireVsCodeApi<T = unknown>(): {
  postMessage(message: any): void;
  setState(state: T): void;
  getState(): T | undefined;
};

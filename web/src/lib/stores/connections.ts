import { writable } from 'svelte/store';

interface ConnectionConfig {
  init?: string;
  properties: Record<string, any>;
  "log-queries"?: boolean;
  "log-parameters"?: boolean;
  allow?: string;
}

function createConnectionsStore() {
  const { subscribe, set, update } = writable<Record<string, ConnectionConfig>>({});

  return {
    subscribe,
    update: (name: string, config: ConnectionConfig) => {
      update(connections => ({
        ...connections,
        [name]: config
      }));
    },
    remove: (name: string) => {
      update(connections => {
        const { [name]: _, ...rest } = connections;
        return rest;
      });
    },
    reset: () => set({})
  };
}

export const connections = createConnectionsStore(); 
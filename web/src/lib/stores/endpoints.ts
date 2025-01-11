import { writable } from 'svelte/store';
import type { EndpointConfig } from '$lib/types/config';

function createEndpointsStore() {
  const { subscribe, set, update } = writable<Record<string, EndpointConfig>>({});

  return {
    subscribe,
    update: (config: EndpointConfig) => {
      update(endpoints => ({
        ...endpoints,
        [config.path]: config
      }));
    },
    remove: (path: string) => {
      update(endpoints => {
        const { [path]: _, ...rest } = endpoints;
        return rest;
      });
    },
    reset: () => set({})
  };
}

export const endpoints = createEndpointsStore(); 
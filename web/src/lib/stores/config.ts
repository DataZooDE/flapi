import { writable } from 'svelte/store';

export const activeConfigView = writable<'request' | 'query' | 'cache' | 'connection'>('request');

import { writable, derived } from 'svelte/store';

// Define route types
export interface Route {
  path: string;
  component: any;
}

// Create stores
export const currentPath = writable(window.location.pathname);
export const routes = writable<Route[]>([]);

// Update path on navigation
window.addEventListener('popstate', () => {
  currentPath.set(window.location.pathname);
});

// Router component
export { default as Router } from './router.svelte';

// Navigation function
export function navigate(path: string) {
  window.history.pushState({}, '', path);
  currentPath.set(path);
} 
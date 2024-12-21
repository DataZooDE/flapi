/// <reference types="@sveltejs/kit" />
/// <reference types="svelte" />
/// <reference types="vite/client" />

declare module '$app/stores' {
  import { type Readable } from 'svelte/store';
  import { type Page } from '@sveltejs/kit';
  export const page: Readable<Page>;
}

declare module 'lucide-svelte' {
  import { SvelteComponentTyped } from 'svelte';
  export class Home extends SvelteComponentTyped<{ class?: string }> {}
  export class Database extends SvelteComponentTyped<{ class?: string }> {}
  export class Network extends SvelteComponentTyped<{ class?: string }> {}
}

declare namespace App {
  interface Locals {}
  interface PageData {}
  interface Error {}
  interface Platform {}
} 
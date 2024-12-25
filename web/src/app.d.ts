/// <reference types="@sveltejs/kit" />
/// <reference types="svelte" />
/// <reference types="vite/client" />

declare module '$app/stores' {
  import { type Readable } from 'svelte/store';
  import { type Page } from '@sveltejs/kit';
  export const page: Readable<Page>;
}

declare module '$app/navigation' {
  export function goto(url: string): Promise<void>;
}

declare module 'lucide-svelte' {
  import { SvelteComponentTyped } from 'svelte';
  export class Home extends SvelteComponentTyped<{ class?: string }> {}
  export class Database extends SvelteComponentTyped<{ class?: string }> {}
  export class Network extends SvelteComponentTyped<{ class?: string }> {}
  export class File extends SvelteComponentTyped<{ class?: string }> {}
  export class Cog extends SvelteComponentTyped<{ class?: string }> {}
  export class Terminal extends SvelteComponentTyped<{ class?: string }> {}
  export class Save extends SvelteComponentTyped<{ class?: string }> {}
  export class Play extends SvelteComponentTyped<{ class?: string }> {}
  export class Trash extends SvelteComponentTyped<{ class?: string }> {}
  export class Settings extends SvelteComponentTyped<{ class?: string }> {}
  export class Code extends SvelteComponentTyped<{ class?: string }> {}
}

declare module '@codemirror/view' {
  export const EditorView: any;
  export const keymap: any;
}

declare module '@codemirror/lang-sql' {
  export const sql: any;
}

declare module '@codemirror/theme-one-dark' {
  export const oneDark: any;
}

declare module '@codemirror/commands' {
  export const defaultKeymap: any;
}

declare module '@codemirror/language' {
  export const syntaxHighlighting: any;
}

declare namespace App {
  interface Locals {}
  interface PageData {}
  interface Error {}
  interface Platform {}
} 
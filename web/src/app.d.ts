/// <reference types="@sveltejs/kit" />

declare namespace App {
  // interface Error {}
  // interface Locals {}
  // interface PageData {}
  // interface Platform {}
}

declare namespace svelteHTML {
  interface HTMLAttributes<T> {
    'on:click'?: (event: CustomEvent<T> & { target: EventTarget & T }) => void;
    'on:input'?: (event: CustomEvent<T> & { target: EventTarget & T }) => void;
    'on:change'?: (event: CustomEvent<T> & { target: EventTarget & T }) => void;
  }
} 
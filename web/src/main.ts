import './app.css'
import { mount } from 'svelte';
import App from './App.svelte'

try {
  const target = document.getElementById("app");
  if (!target) {
    throw new Error("Could not find app element");
  }
  const app = mount(App, { target });
} catch (error: any) {
  document.body.innerHTML = `<div style="color: red; padding: 2rem;">
    <h1>Error</h1>
    <pre>${error?.message || 'Unknown error'}</pre>
  </div>`
} 
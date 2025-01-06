import './app.css'
import { mount } from 'svelte';
import App from './App.svelte'

try {
  const app = mount(App, { target: document.getElementById("app") });
} catch (error) {
  document.body.innerHTML = `<div style="color: red; padding: 2rem;">
    <h1>Error</h1>
    <pre>${error.message}</pre>
  </div>`
}

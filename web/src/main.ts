import { mount } from 'svelte';
import './app.css';
import App from './App.svelte';

const target = document.getElementById('app');
if (!target) {
  throw new Error('Could not find app element');
}

try {
  const app = mount(App, { target: target, props: { } });
} catch (error: any) {
  console.error('Failed to mount app:', error);
  target.innerHTML = `
    <div class="p-4 text-red-500">
      <h1 class="text-xl font-bold mb-2">Error</h1>
      <pre class="whitespace-pre-wrap">${error?.message || 'Unknown error'}</pre>
    </div>
  `;
} 
import { createInertiaApp } from '@inertiajs/svelte'

export function start({ target }) {
  createInertiaApp({
    target,
    resolve: name => {
      const pages = import.meta.glob('./routes/**/*.svelte', { eager: true })
      return pages[`./routes/${name}.svelte`]
    },
    setup({ el, App, props }) {
      new App({ target: el, props })
    },
  })
} 
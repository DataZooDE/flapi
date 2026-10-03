import { randomBytes } from 'node:crypto';

/** Escape text for use in HTML element content or a quoted attribute value. */
export function escapeHtmlText(value: unknown): string {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

export function makeNonce(): string {
  return randomBytes(16).toString('base64').replace(/[+/=]/g, '');
}

/**
 * A strict Content-Security-Policy for a webview: nothing by default, scripts only
 * with the page's nonce, styles from the webview origin plus inline (the pages
 * carry inline style attributes) and the codicon stylesheet host.
 */
export function buildCsp(cspSource: string, nonce: string): string {
  return [
    "default-src 'none'",
    `style-src ${cspSource} 'unsafe-inline' https://cdn.jsdelivr.net`,
    `font-src ${cspSource} https://cdn.jsdelivr.net`,
    `img-src ${cspSource} data:`,
    `script-src 'nonce-${nonce}'`,
  ].join('; ');
}

/**
 * Inline `onclick="fn()"` attributes cannot run under that policy. Elements carry
 * `data-onclick="fn"` (and optionally `data-arg="x"` or `data-pass="this"`) and
 * this one delegated listener calls the page function of that name.
 */
export const ACTION_BRIDGE_JS = `
(function () {
  function dispatch(el, kind) {
    var name = el.getAttribute('data-' + kind);
    var fn = name && window[name];
    if (typeof fn !== 'function') { return; }
    if (el.getAttribute('data-pass') === 'this') { fn(el); }
    else if (el.hasAttribute('data-arg')) { fn(el.getAttribute('data-arg')); }
    else { fn(); }
  }
  ['click', 'change'].forEach(function (kind) {
    document.addEventListener(kind, function (e) {
      var el = e.target && e.target.closest ? e.target.closest('[data-on' + kind + ']') : null;
      if (el && !el.disabled) { dispatch(el, 'on' + kind); }
    });
  });
})();
`;

/**
 * The endpoint tester webview renders what the SERVER returns - column names,
 * error text - and what the config says (url-path, method). All of it must be
 * shown as text. Driven end to end: a real flAPI endpoint returns a hostile
 * column name, the real EndpointTestService fetches it, the real panel builds
 * its page and the page's own script renders it (in jsdom).
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { JSDOM } from 'jsdom';
import { createApiClient, buildEndpointUrl } from '@flapi/shared';
import { EndpointTesterPanel } from '../../src/webview/endpointTesterPanel';
import { EndpointTestService } from '../../src/services/endpointTestService';
import { ParameterStorageService } from '../../src/services/parameterStorage';
import { SqlTemplateTesterPanel } from '../../src/webview/sqlTemplateTesterPanel';
import { TestStateService } from '../../src/services/testStateService';
import { fakeContext, panels, Uri } from '../stubs/vscode';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const client = createApiClient({ baseUrl: base, authToken: token, timeout: 30, retries: 0, verifyTls: true } as never);

const HOSTILE = '<img src=x onerror="window.__xss=1">';

async function openPanel(endpointConfig: Record<string, unknown>, urlPath: string) {
  const context = fakeContext();
  const lines: string[] = [];
  const service = new EndpointTestService({ appendLine: (l: string) => lines.push(l) } as never);
  const storage = new ParameterStorageService(context as never);
  panels.length = 0;
  EndpointTesterPanel.currentPanel = undefined; // createOrShow reuses a live panel
  await EndpointTesterPanel.createOrShow(Uri.file('/ext') as never, storage, service, endpointConfig, base);
  const panel = panels[0];

  const dom = new JSDOM(panel.webview.html, {
    url: 'http://localhost/',
    runScripts: 'dangerously',
    pretendToBeVisual: true,
    beforeParse(window) {
      (window as any).acquireVsCodeApi = () => ({
        postMessage: (m: unknown) => panel.receive?.(m),
        getState: () => undefined,
        setState: () => undefined,
      });
    },
  });
  panel.toPage = (m) => {
    dom.window.dispatchEvent(new dom.window.MessageEvent('message', { data: m }));
  };
  // Initial state, as the panel itself would send it.
  await panel.webview.postMessage({
    command: 'init',
    config: endpointConfig,
    state: { slug: urlPath, baseUrl: base, method: 'GET', parameters: {}, headers: {}, history: [] },
  });
  return { dom, panel, lines, context };
}

const urlPath = '/xss-probe';

beforeAll(async () => {
  const def = {
    'url-path': urlPath, method: 'GET', 'template-source': 'xss-probe.sql',
    connection: ['customers-parquet'],
  };
  await client.post('/api/v1/_config/endpoints', def);
  await client.put(buildEndpointUrl(urlPath, 'template'), {
    template: `SELECT 1 AS "${HOSTILE.replace(/"/g, '""')}", 2 AS ok`,
  });
});

afterAll(async () => {
  await client.delete(buildEndpointUrl(urlPath));
});

describe('endpoint tester webview', () => {
  it('shows a hostile column name returned by the server as text', async () => {
    const { dom, panel } = await openPanel({ 'url-path': urlPath, method: 'GET' }, urlPath);
    // Run the request through the page exactly as the Send button does.
    await panel.receive?.({ command: 'test', data: { parameters: {}, headers: {}, body: undefined, authConfig: undefined } });
    await new Promise((r) => setTimeout(r, 300));

    const doc = dom.window.document;
    expect(doc.querySelectorAll('img').length, 'a server-supplied column name became an element').toBe(0);
    expect((dom.window as any).__xss).toBeUndefined();
    expect(doc.body.textContent).toContain(HOSTILE);
  });

  it('shows a hostile url-path and method from the configuration as text', async () => {
    const { dom } = await openPanel({ 'url-path': `/<img src=x onerror="window.__xss=2">`, method: 'GET<b id=injected>' }, urlPath);
    const doc = dom.window.document;
    expect(doc.querySelectorAll('img').length).toBe(0);
    expect(doc.getElementById('injected')).toBeNull();
    expect((dom.window as any).__xss).toBeUndefined();
  });

  it('has a Content-Security-Policy that forbids inline script without the nonce', async () => {
    const { panel } = await openPanel({ 'url-path': urlPath, method: 'GET' }, urlPath);
    const html = panel.webview.html;
    const csp = /<meta http-equiv="Content-Security-Policy" content="([^"]+)"/.exec(html)?.[1] ?? '';
    expect(csp).toContain("default-src 'none'");
    expect(csp).toMatch(/script-src[^;]*'nonce-[A-Za-z0-9+/=_-]+'/);
    expect(csp).not.toMatch(/script-src[^;]*'unsafe-inline'/);
    const nonce = /'nonce-([^']+)'/.exec(csp)?.[1];
    expect(html).toContain(`<script nonce="${nonce}"`);
  });
});

describe('endpoint tester: the page still works without inline handlers', () => {
  it('a collapsible header toggles through the delegated data-onclick bridge', async () => {
    const { dom } = await openPanel({ 'url-path': '/xss-probe', method: 'GET' }, '/xss-probe');
    const doc = dom.window.document;
    expect(doc.querySelector('[onclick]'), 'an inline onclick handler would be blocked by the CSP').toBeNull();
    const header = doc.querySelector('[data-onclick="toggleSection"][data-arg="headers"]') as HTMLElement;
    const content = doc.getElementById('headers-content')!;
    const before = content.classList.contains('expanded');
    header.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
    expect(content.classList.contains('expanded')).toBe(!before);
  });
});

describe('SQL template tester webview', () => {
  async function openSqlPanel() {
    const context = fakeContext();
    panels.length = 0;
    (SqlTemplateTesterPanel as any).currentPanel = undefined;
    await SqlTemplateTesterPanel.createOrShow(
      Uri.file('/ext') as never,
      new TestStateService(context as never),
      client,
      Uri.file(`${process.env.FLAPI_IT_WORKDIR}/examples/sqls/xss-probe.sql`) as never,
      '/work/xss-probe.yaml',
    );
    const panel = panels[0];
    const dom = new JSDOM(panel.webview.html, {
      url: 'http://localhost/',
      runScripts: 'dangerously',
      beforeParse(window) {
        (window as any).acquireVsCodeApi = () => ({
          postMessage: (m: unknown) => panel.receive?.(m),
          getState: () => undefined,
          setState: () => undefined,
        });
      },
    });
    panel.toPage = (m) => {
      dom.window.dispatchEvent(new dom.window.MessageEvent('message', { data: m }));
    };
    return { dom, panel };
  }

  it('renders the server\'s hostile column name as text in the JSON and table views', async () => {
    const { dom, panel } = await openSqlPanel();
    await panel.receive?.({ command: 'test', data: { parameters: {}, limit: 10 } });
    await new Promise((r) => setTimeout(r, 400));
    const doc = dom.window.document;
    expect(doc.querySelectorAll('img').length, 'a server-supplied column name became an element').toBe(0);
    expect((dom.window as any).__xss).toBeUndefined();
  });

  it('carries a CSP with a nonce and no inline handlers', async () => {
    const { dom, panel } = await openSqlPanel();
    const html = panel.webview.html;
    expect(html).toMatch(/Content-Security-Policy" content="default-src 'none'; [^"]*script-src 'nonce-/);
    expect(dom.window.document.querySelector('[onclick],[onchange]')).toBeNull();
  });
});

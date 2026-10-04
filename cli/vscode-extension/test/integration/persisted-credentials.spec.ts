/**
 * Credentials typed into the endpoint tester (Authorization, API keys, tokens, in
 * headers or parameters) must never reach workspaceState: it is an ordinary state
 * file beside the workspace. The live value stays in the open panel and is sent to
 * the server; after the panel is reopened the NAME is restored with an empty value.
 * Driven through the real panel, test service and storage against a real server
 * whose /customers/ endpoint requires basic auth.
 */
import { describe, it, expect } from 'vitest';
import { EndpointTesterPanel } from '../../src/webview/endpointTesterPanel';
import { EndpointTestService } from '../../src/services/endpointTestService';
import { ParameterStorageService } from '../../src/services/parameterStorage';
import { TestStateService } from '../../src/services/testStateService';
import { fakeContext, panels, Uri } from '../stubs/vscode';

const base = process.env.FLAPI_BASE_URL!;
const BASIC = Buffer.from('admin:secret').toString('base64');
const AUTH = `Basic ${BASIC}`;

async function open(context: ReturnType<typeof fakeContext>) {
  panels.length = 0;
  EndpointTesterPanel.currentPanel = undefined;
  const lines: string[] = [];
  const service = new EndpointTestService({ appendLine: (l: string) => lines.push(l) } as never);
  const storage = new ParameterStorageService(context as never);
  await EndpointTesterPanel.createOrShow(Uri.file('/ext') as never, storage, service, { 'url-path': '/customers/', method: 'GET' }, base);
  return { panel: panels[0], storage };
}

describe('credentials typed into the tester are not persisted', () => {
  it('headers: sent to the real server, absent from workspaceState, name restored empty', async () => {
    const context = fakeContext();
    const { panel, storage } = await open(context);
    const headers = { Authorization: AUTH, 'X-Api-Key': 'k-987654', 'X-Trace': 'visible' };
    await panel.receive?.({ command: 'updateHeaders', data: headers });

    // Used for real: the request authenticates against the real server.
    let status = 0;
    const posted: any[] = [];
    panel.toPage = (m) => posted.push(m);
    await panel.receive?.({ command: 'test', data: { parameters: {}, headers, body: undefined, authConfig: undefined } });
    status = posted.find((m) => m.command === 'testResponse')?.response.status;
    expect(status).toBe(200);

    const stored = context.workspaceState.dump();
    expect(stored).not.toContain(BASIC);
    expect(stored).not.toContain('k-987654');

    const restored = storage.getEndpointState('/customers/')!;
    expect(restored.headers.Authorization).toBe('');
    expect(restored.headers['X-Api-Key']).toBe('');
    expect(restored.headers['X-Trace']).toBe('visible');
  });

  it('parameters with credential-looking names are not persisted either', async () => {
    const context = fakeContext();
    const { panel, storage } = await open(context);
    await panel.receive?.({ command: 'updateParams', data: { api_key: 'p-123456', id: '1' } });
    expect(context.workspaceState.dump()).not.toContain('p-123456');
    expect(storage.getEndpointState('/customers/')!.parameters.id).toBe('1');
  });

  it('workspace default headers are not persisted with credentials', async () => {
    const context = fakeContext();
    const storage = new ParameterStorageService(context as never);
    await storage.setWorkspaceDefaults({ Authorization: AUTH, Accept: 'application/json' });
    expect(context.workspaceState.dump()).not.toContain(BASIC);
    expect(storage.getWorkspaceDefaults().Accept).toBe('application/json');
  });

  it('TestStateService (the SQL tester and REST state) strips them too', async () => {
    const context = fakeContext();
    const svc = new TestStateService(context as never);
    await svc.setWorkspaceDefaults({ Authorization: AUTH });
    const state = await svc.initializeRestState('/customers/', base, 'GET', {}, { Authorization: AUTH });
    await svc.updateHeaders('/customers/', { Authorization: AUTH, 'X-Trace': 't' });
    expect(context.workspaceState.dump()).not.toContain(BASIC);
    expect(state === undefined || typeof state === 'object').toBe(true);
  });
});

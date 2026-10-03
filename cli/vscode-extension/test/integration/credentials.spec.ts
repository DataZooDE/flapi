/**
 * Credentials typed into the endpoint tester must reach the server but never the
 * output channel or the persisted request history (workspaceState).
 * Run against the real /customers/ endpoint, which requires basic auth.
 */
import { describe, it, expect } from 'vitest';
import { EndpointTestService } from '../../src/services/endpointTestService';

const base = process.env.FLAPI_BASE_URL!;
const BASIC = Buffer.from('admin:secret').toString('base64');

function service() {
  const lines: string[] = [];
  return { lines, svc: new EndpointTestService({ appendLine: (l: string) => lines.push(l) } as never) };
}

describe('EndpointTestService and credentials', () => {
  it('sends the credential to the server (it authenticates)', async () => {
    const { svc } = service();
    const unauth = await svc.executeRequest(base, '/customers/', 'GET', {}, {});
    expect(unauth.status).toBe(401);
    const ok = await svc.executeRequest(base, '/customers/', 'GET', {}, { Authorization: `Basic ${BASIC}` });
    expect(ok.status).toBe(200);
  });

  it('does not write the credential to the output channel', async () => {
    const { svc, lines } = service();
    await svc.executeRequest(base, '/customers/', 'GET', {}, { Authorization: `Basic ${BASIC}`, 'X-Trace': 'visible' });
    const log = lines.join('\n');
    expect(log).not.toContain(BASIC);
    expect(log).toContain('X-Trace'); // non-sensitive headers are still logged
  });

  it('does not keep the credential in the request history', async () => {
    const { svc } = service();
    const headers = { Authorization: `Basic ${BASIC}`, 'X-Api-Key': 'k-123456', Accept: 'application/json' };
    const response = await svc.executeRequest(base, '/customers/', 'GET', {}, headers);
    const entry = svc.createHistoryEntry({}, headers, response);
    const stored = JSON.stringify(entry);
    expect(stored).not.toContain(BASIC);
    expect(stored).not.toContain('k-123456');
    expect(entry.headers.Accept).toBe('application/json');
  });
});

/**
 * One ConfigService transport. FlapiApiClient used to build its own axios instance
 * (no retry policy, no TLS switch, seconds-vs-ms timeout, a different error type),
 * so token, TLS, timeout and retry behaviour diverged from the CLI's client.
 * Everything here runs against the REAL flapi server; the only network fault
 * injection is a TCP/TLS hop in front of it.
 */
import { describe, it, expect, afterEach } from 'vitest';
import { FlapiApiClient, createApiClient, AxiosResponseError } from '@flapi/shared';
import { EndpointTestService } from '../../src/services/endpointTestService';
import { lossyProxy, blackhole, tlsFront } from './netproxy';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const out = { appendLine: () => {} } as never;

const closers: Array<() => Promise<void>> = [];
afterEach(async () => {
  while (closers.length) await closers.pop()!();
});
const track = <T extends { close: () => Promise<void> }>(x: T) => {
  closers.push(x.close);
  return x;
};

describe('(a) token handling matches createApiClient', () => {
  it('authenticates with a token', async () => {
    const api = new FlapiApiClient({ baseURL: base, token });
    expect(await api.listEndpoints()).toBeTruthy();
  });

  it('rejects a wrong token with the same error type as createApiClient', async () => {
    const shared = createApiClient({ baseUrl: base, timeout: 30, retries: 0, verifyTls: true, authToken: 'wrong' } as never);
    const sharedErr = await shared.get('/api/v1/_config/endpoints').catch((e) => e);
    const api = new FlapiApiClient({ baseURL: base, token: 'wrong' });
    const err = await api.listEndpoints().then(() => undefined, (e) => e);
    expect(sharedErr).toBeInstanceOf(AxiosResponseError);
    expect(err).toBeInstanceOf(AxiosResponseError);
    expect(err.response.status).toBe(sharedErr.response.status);
    expect([401, 403]).toContain(err.response.status);
  });

  it('accepts the shared option names (baseUrl/authToken) as well as the legacy ones', async () => {
    const api = new FlapiApiClient({ baseUrl: base, authToken: token } as never);
    expect(await api.listEndpoints()).toBeTruthy();
  });

  it('setToken swaps the credential', async () => {
    const api = new FlapiApiClient({ baseURL: base, token: 'wrong' });
    await expect(api.listEndpoints()).rejects.toBeInstanceOf(AxiosResponseError);
    api.setToken(token);
    expect(await api.listEndpoints()).toBeTruthy();
  });
});

describe('(b) retry policy: reads yes, writes never', () => {
  it('does not repeat a POST whose response was lost', async () => {
    const proxy = track(await lossyProxy(base, 'POST'));
    const api = new FlapiApiClient({ baseURL: proxy.url, token, retries: 3 } as never);
    await expect(api.reloadEndpointConfig('customers')).rejects.toBeTruthy();
    await new Promise((r) => setTimeout(r, 1500)); // a (wrong) retry would have landed by now
    expect(proxy.count('POST')).toBe(1);
  });

  it('does retry a GET whose response was lost', async () => {
    const proxy = track(await lossyProxy(base, 'GET'));
    const api = new FlapiApiClient({ baseURL: proxy.url, token, retries: 2 } as never);
    expect(await api.listEndpoints()).toBeTruthy();
    expect(proxy.count('GET')).toBe(2);
  });
});

describe('(c) timeout and TLS options reach the transport', () => {
  it('times out after `timeout` seconds against a server that never answers', async () => {
    const hole = track(await blackhole());
    const api = new FlapiApiClient({ baseURL: hole.url, token, timeout: 1, retries: 0 } as never);
    const t0 = Date.now();
    const err = await api.listEndpoints().then(() => undefined, (e) => e);
    const took = Date.now() - t0;
    expect(err).toBeTruthy();
    expect(err.code).toBe('ECONNABORTED');
    expect(took).toBeGreaterThanOrEqual(900);
    expect(took).toBeLessThan(5000);
  });

  it('verifies certificates by default and skips verification with verifyTls:false', async () => {
    const front = track(await tlsFront(base));
    const strict = new FlapiApiClient({ baseURL: front.url, token, retries: 0 } as never);
    await expect(strict.listEndpoints()).rejects.toBeTruthy();
    const insecure = new FlapiApiClient({ baseURL: front.url, token, retries: 0, verifyTls: false } as never);
    expect(await insecure.listEndpoints()).toBeTruthy();
  });

  it('reconfigure() applies new TLS/timeout settings to the same client object', async () => {
    const front = track(await tlsFront(base));
    const api = new FlapiApiClient({ baseURL: front.url, token, retries: 0 } as never);
    await expect(api.listEndpoints()).rejects.toBeTruthy();
    (api as any).reconfigure({ baseURL: front.url, token, retries: 0, verifyTls: false });
    expect(await api.listEndpoints()).toBeTruthy();
  });
});

describe('(d) endpointTestService hits REST routes with the same transport settings, never the config token', () => {
  it('never sends X-Config-Token or the config token', async () => {
    const proxy = track(await lossyProxy(base, 'NONE'));
    const svc = new EndpointTestService(out);
    const res = await svc.executeRequest(proxy.url, '/customers/', 'GET', {}, {}, undefined, { type: 'none' } as never);
    expect(res.status).not.toBe(0);
    expect(proxy.heads.length).toBeGreaterThan(0);
    for (const h of proxy.heads) {
      expect(h.toLowerCase()).not.toContain('x-config-token');
      expect(h).not.toContain(token);
    }
  });

  it('honours the insecure setting', async () => {
    const front = track(await tlsFront(base));
    const strict = new EndpointTestService(out, () => ({ timeout: 30, verifyTls: true }));
    expect((await strict.executeRequest(front.url, '/customers/', 'GET', {}, {})).status).toBe(0);
    const insecure = new EndpointTestService(out, () => ({ timeout: 30, verifyTls: false }));
    expect((await insecure.executeRequest(front.url, '/customers/', 'GET', {}, {})).status).not.toBe(0);
  });

  it('honours the timeout setting', async () => {
    const hole = track(await blackhole());
    const svc = new EndpointTestService(out, () => ({ timeout: 1, verifyTls: true }));
    const t0 = Date.now();
    const res = await Promise.race([
      svc.executeRequest(hole.url, '/customers/', 'GET', {}, {}),
      new Promise<'hung'>((r) => setTimeout(() => r('hung'), 6000)),
    ]);
    expect(res).not.toBe('hung');
    expect((res as any).status).toBe(0);
    expect(Date.now() - t0).toBeLessThan(5000);
  });
});

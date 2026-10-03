/**
 * The wizard's output must produce an endpoint a REAL flAPI server accepts and
 * serves. It used to send {endpoint_name, url_path, table, parameters}, which the
 * server cannot deserialize, and wrote a YAML file in the same wrong shape.
 */
import { describe, it, expect, afterAll } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createApiClient, buildEndpointUrl } from '@flapi/shared';
import { createEndpointViaApi, saveToFile } from '../../src/commands/endpoints/wizard';
import { flapii } from './helpers';
import type { WizardConfig } from '../../src/lib/types';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;

const client = createApiClient({
  baseUrl: base,
  authToken: token,
  timeout: 30,
  retries: 0,
  verifyTls: true,
} as never);

const config: WizardConfig = {
  endpoint_name: 'wizard-probe',
  url_path: '/wizard-probe',
  description: 'created by the wizard test',
  connection: 'customers-parquet',
  // The wizard's contract is a plain table identifier on the connection.
  table: 'customers',
  method: 'GET',
  parameters: [{ name: 'c_custkey', type: 'integer', required: false, location: 'query', validators: [] }],
  enable_cache: false,
};

describe('wizard output against a real server', () => {
  afterAll(async () => {
    await flapii(['endpoints', 'delete', '/wizard-probe']);
  });

  it('refuses a table name that is not a plain identifier', async () => {
    await expect(
      createEndpointViaApi({ client } as never, { ...config, table: "x'; DROP TABLE y; --" }),
    ).rejects.toThrow(/plain identifier/);
  });

  it('the endpoint file the wizard writes is accepted by the server validator', async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-wizard-'));
    const yamlFile = path.join(dir, 'probe.yaml');
    await saveToFile(config, yamlFile);
    expect(fs.existsSync(path.join(dir, 'probe.sql'))).toBe(true);
    const r = await flapii(['endpoints', 'validate', '/wizard-probe', '--file', yamlFile, '--output', 'json']);
    expect(r.exitCode, r.stderr + r.stdout).toBe(0);
    expect(JSON.parse(r.stdout.slice(r.stdout.indexOf('{'))).valid).toBe(true);
  });

  it('createEndpointViaApi makes the server create the endpoint and its template', async () => {
    await createEndpointViaApi({ client } as never, config);
    const got = await client.get(buildEndpointUrl('/wizard-probe'));
    expect(got.data.urlPath ?? got.data['url-path']).toBe('/wizard-probe');
    const tpl = await client.get(buildEndpointUrl('/wizard-probe', 'template'));
    expect(tpl.data.template).toContain('SELECT * FROM customers');
    expect(tpl.data.template).toContain('{{ params.c_custkey }}');
  });
});

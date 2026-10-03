/**
 * "Reload" after saving an endpoint YAML must reload the endpoint the server knows,
 * and say so truthfully. It guessed the slug from the FILE NAME (customers-rest.yaml
 * -> "customers-rest"), but the server's slug comes from the url-path, so the reload
 * of nearly every real endpoint was a 404 that was shown as a warning, while the
 * "SQL template reloaded" message appeared regardless.
 */
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import path from 'node:path';
import { FlapiApiClient } from '@flapi/shared';
import { YamlValidator } from '../../src/validation/yamlValidator';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const sqls = path.join(process.env.FLAPI_IT_WORKDIR!, 'examples', 'sqls');

function validator() {
  const lines: string[] = [];
  const api = new FlapiApiClient({ baseURL: base, token });
  const v = new YamlValidator(api as never, { appendLine: (l: string) => lines.push(l) } as never);
  return { v, lines };
}

const docFor = (file: string) => ({
  uri: { fsPath: file },
  fileName: file,
  getText: () => fs.readFileSync(file, 'utf8'),
});

describe('YamlValidator.reloadEndpointConfig against a real server', () => {
  it('reloads an endpoint whose file name differs from its url-path slug', async () => {
    const { v, lines } = validator();
    const ok = await v.reloadEndpointConfig(docFor(path.join(sqls, 'customers', 'customers-rest.yaml')) as never);
    expect(ok, lines.join('\n')).toBe(true);
  });

  it('reports failure when the server does not know the endpoint', async () => {
    const { v } = validator();
    const file = path.join(sqls, 'ghost-endpoint.yaml');
    fs.writeFileSync(file, 'url-path: /ghost-that-is-not-loaded\nmethod: GET\ntemplate-source: g.sql\nconnection: [customers-parquet]\n');
    try {
      expect(await v.reloadEndpointConfig(docFor(file) as never)).toBe(false);
    } finally {
      fs.rmSync(file, { force: true });
    }
  });
});

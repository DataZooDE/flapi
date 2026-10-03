/**
 * "Validate SQL Template" validates the editor's UNSAVED text on a real server.
 */
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import path from 'node:path';
import { createApiClient } from '@flapi/shared';
import { validateTemplateText, describeValidation } from '../../src/validation/templateValidator';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const client = createApiClient({ baseUrl: base, authToken: token, timeout: 30, retries: 0, verifyTls: true } as never);
const slug = '/customers/';
const sqlFile = path.join(process.env.FLAPI_IT_WORKDIR!, 'examples', 'sqls', 'customers', 'customers.sql');

describe('validateTemplateText', () => {
  it('accepts valid unsaved SQL', async () => {
    const result = await validateTemplateText(client, slug, "SELECT 1 AS n");
    expect(result.valid).toBe(true);
    expect(describeValidation(result).ok).toBe(true);
  });

  it('rejects broken unsaved SQL although the saved file is fine, and says why', async () => {
    const saved = fs.readFileSync(sqlFile, 'utf8');
    const result = await validateTemplateText(client, slug, 'SELEC * FORM nowhere');
    expect(result.valid).toBe(false);
    expect(result.errors.length).toBeGreaterThan(0);
    const shown = describeValidation(result);
    expect(shown.ok).toBe(false);
    expect(shown.message).toContain('invalid');
    expect(fs.readFileSync(sqlFile, 'utf8')).toBe(saved); // nothing was written
  });

  it('rejects an unterminated Mustache tag', async () => {
    const result = await validateTemplateText(client, slug, 'SELECT {{#open');
    expect(result.valid).toBe(false);
  });

  it('validates the real customers template text as it is on disk', async () => {
    const result = await validateTemplateText(client, slug, fs.readFileSync(sqlFile, 'utf8'));
    expect(result.valid, JSON.stringify(result.errors)).toBe(true);
  });
});

import { endpointPathFromYamlText } from '../../src/validation/endpointPath';

describe('endpointPathFromYamlText', () => {
  it('reads the url-path of the real customers endpoint file (which carries {{include}} directives)', () => {
    const file = path.join(process.env.FLAPI_IT_WORKDIR!, 'examples', 'sqls', 'customers', 'customers-rest.yaml');
    expect(endpointPathFromYamlText(fs.readFileSync(file, 'utf8'))).toBe('/customers/');
  });
  it('is null for a file with no url-path', () => {
    expect(endpointPathFromYamlText('mcp-tool:\n  name: x\n')).toBeNull();
  });
});

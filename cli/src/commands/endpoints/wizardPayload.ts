/**
 * Turns a WizardConfig into what flAPI actually reads.
 *
 * The wizard used to send `endpoint_name`/`url_path`/`table`/`parameters`, which
 * the server cannot deserialize (it needs `url-path`, `template-source`,
 * `request`, ...), and wrote a YAML file in the same wrong shape - so completing
 * the wizard could not produce a working endpoint. These builders are pure and
 * are exercised against a real server in test/integration/wizard.spec.ts.
 */
import yaml from 'js-yaml';
import type { ParameterDefinition, ValidatorConfig, WizardConfig } from '../../lib/types';

const SAFE_IDENTIFIER = /^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*$/;

export function slugOf(urlPath: string): string {
  const slug = urlPath.replace(/[^A-Za-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
  return slug || 'endpoint';
}

export function templateSourceOf(config: WizardConfig): string {
  return `${slugOf(config.url_path)}.sql`;
}

function validatorsFor(param: ParameterDefinition): Record<string, unknown>[] {
  const out: Record<string, unknown>[] = [];
  const extra = (param.validators ?? []) as ValidatorConfig[];
  if (param.type === 'integer') {
    const v: Record<string, unknown> = { type: 'int' };
    for (const e of extra) {
      if (e.type === 'min' && typeof e.value === 'number') v.min = e.value;
      if (e.type === 'max' && typeof e.value === 'number') v.max = e.value;
    }
    out.push(v);
  } else if (param.type === 'string') {
    const v: Record<string, unknown> = { type: 'string' };
    for (const e of extra) {
      if (e.type === 'minLength' && typeof e.value === 'number') v['min-length'] = e.value;
      if (e.type === 'maxLength' && typeof e.value === 'number') v['max-length'] = e.value;
      if (e.type === 'pattern' && typeof e.value === 'string') v.pattern = e.value;
    }
    if (v['max-length'] === undefined) v['max-length'] = 200;
    out.push(v);
    for (const e of extra) {
      if (e.type === 'enum' && Array.isArray(e.value)) out.push({ type: 'enum', values: e.value });
    }
  }
  return out;
}

function requestField(param: ParameterDefinition): Record<string, unknown> {
  const field: Record<string, unknown> = {
    'field-name': param.name,
    'field-in': param.location,
    'field-type': param.type,
    required: param.required,
  };
  if (param.description) field.description = param.description;
  if (param.defaultValue !== undefined) field.default = param.defaultValue;
  const validators = validatorsFor(param);
  if (validators.length > 0) field.validators = validators;
  return field;
}

/** The endpoint definition, in the hyphenated shape the config service reads. */
export function buildEndpointDefinition(config: WizardConfig): Record<string, unknown> {
  const slug = slugOf(config.url_path);
  const def: Record<string, unknown> = {
    'url-path': config.url_path,
    method: config.method,
    'template-source': templateSourceOf(config),
    connection: [config.connection],
  };
  if (config.parameters.length > 0) def.request = config.parameters.map(requestField);
  if (config.enable_cache) {
    // flAPI refreshes on a schedule; it has no TTL. The wizard's TTL (seconds)
    // becomes the refresh interval.
    def.cache = { enabled: true, table: `${slug}_cache`, schedule: `${config.cache_ttl || 300}s` };
  }
  return def;
}

/**
 * SELECT * FROM <table>, with one optional filter per parameter. Values use the
 * double-brace form, which flAPI binds as prepared-statement parameters rather
 * than splicing into the SQL text.
 */
export function buildTemplateSql(config: WizardConfig): string {
  if (!SAFE_IDENTIFIER.test(config.table)) {
    throw new Error(`Table name "${config.table}" is not a plain identifier (letters, digits, underscores, dots).`);
  }
  const filters = config.parameters
    .filter((p) => p.location !== 'body')
    .map((p) => {
      if (!SAFE_IDENTIFIER.test(p.name) || p.name.includes('.')) {
        throw new Error(`Parameter name "${p.name}" is not a plain identifier.`);
      }
      return `{{#params.${p.name}}}\n  AND ${p.name} = {{ params.${p.name} }}\n{{/params.${p.name}}}`;
    })
    .join('\n');
  return `SELECT * FROM ${config.table}\nWHERE 1=1\n${filters}${filters ? '\n' : ''}LIMIT 100\n`;
}

/** The YAML file for --output-file; its template is written next to it. */
export function buildEndpointYaml(config: WizardConfig, templateSource: string): string {
  return yaml.dump({ ...buildEndpointDefinition(config), 'template-source': templateSource }, { indent: 2 });
}

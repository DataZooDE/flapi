import { buildEndpointUrl } from '@flapi/shared';

/** The part of an axios instance this needs (the shared client and the extension's axios are separate installs). */
export interface HttpPoster {
  post(url: string, data?: unknown): Promise<{ data: any }>;
}

export interface TemplateIssue {
  type: string;
  message: string;
}

export interface TemplateValidation {
  valid: boolean;
  errors: TemplateIssue[];
  warnings: TemplateIssue[];
}

/**
 * Validate SQL template TEXT - the editor's unsaved content - on the server: the
 * Mustache is rendered with the endpoint's connection/environment and the result is
 * EXPLAINed. Nothing is saved. ("Validate SQL Template" used to validate the saved
 * YAML and then say the SQL was valid.)
 */
export async function validateTemplateText(
  client: HttpPoster,
  endpointPath: string,
  sql: string,
  parameters: Record<string, string> = {},
): Promise<TemplateValidation> {
  const response = await client.post(
    `${buildEndpointUrl(endpointPath, 'template/expand')}?validate_only=1`,
    { parameters, template: sql },
  );
  const data = response.data ?? {};
  return {
    valid: data.valid === true,
    errors: Array.isArray(data.errors) ? data.errors : [],
    warnings: Array.isArray(data.warnings) ? data.warnings : [],
  };
}

/** The message shown to the user for a validation result. */
export function describeValidation(result: TemplateValidation): { ok: boolean; message: string } {
  if (result.valid) {
    const extra = result.warnings.length > 0 ? ` (${result.warnings.length} warning(s))` : '';
    return { ok: true, message: `✓ SQL template is valid${extra}: it rendered and the server accepted the query` };
  }
  const first = result.errors[0]?.message ?? 'The server rejected the template';
  return { ok: false, message: `SQL template is invalid: ${first}` };
}

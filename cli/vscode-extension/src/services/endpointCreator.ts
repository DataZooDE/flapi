import { buildEndpointUrl, pathToSlug } from '@flapi/shared';

/** The part of an axios instance this needs (the shared client and the extension's axios are separate installs). */
export interface HttpClient {
  get(url: string): Promise<{ data: any }>;
  post(url: string, data?: unknown): Promise<{ data: any }>;
  put(url: string, data?: unknown): Promise<{ data: any }>;
}

export interface NewEndpointInput {
  /** File stem of the SQL template; the server stores it as `<name>.sql`. */
  name: string;
  /** The endpoint's REST path, e.g. "/customers". */
  urlPath: string;
  /** Name of a connection configured on the server. */
  connection: string;
  method?: string;
  /** Initial SQL template. */
  sql?: string;
}

export interface CreatedEndpoint {
  urlPath: string;
  slug: string;
}

let rpcId = 0;

/**
 * Call a ConfigService tool over MCP. These tools PERSIST: the endpoint YAML (built
 * by the server from structured arguments, never from text we format) and its SQL are
 * written under the SERVER's templates directory. The REST `POST /endpoints` is
 * in-memory only and would not survive a restart.
 */
async function callTool(http: HttpClient, name: string, args: Record<string, unknown>): Promise<any> {
  const response = await http.post('/mcp/jsonrpc', {
    jsonrpc: '2.0',
    id: ++rpcId,
    method: 'tools/call',
    params: { name, arguments: args },
  });
  const body = response.data ?? {};
  if (body.error) {
    const message = String(body.error.message ?? 'request failed').replace(/^Tool execution failed:\s*/, '');
    throw new Error(message);
  }
  const text = body.result?.content?.[0]?.text;
  if (typeof text !== 'string') {
    return body.result;
  }
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

/** Names of the connections configured on the server. */
export async function listConnectionNames(http: HttpClient): Promise<string[]> {
  const response = await http.get('/api/v1/_config/project');
  const connections = response.data?.connections;
  return connections && typeof connections === 'object' ? Object.keys(connections).sort() : [];
}

/**
 * Create an endpoint on the server and give it its SQL. Nothing is written to the
 * local workspace; user text travels as JSON values only. Throws with the server's
 * message when the server refuses (unknown connection, bad path, ...).
 */
export async function createEndpointOnServer(http: HttpClient, input: NewEndpointInput): Promise<CreatedEndpoint> {
  await callTool(http, 'flapi_create_endpoint', {
    path: input.urlPath,
    method: input.method ?? 'GET',
    'template-source': `${input.name}.sql`,
    connection: [input.connection],
  });
  if (input.sql !== undefined) {
    await http.put(buildEndpointUrl(input.urlPath, 'template'), { template: input.sql });
  }
  return { urlPath: input.urlPath, slug: pathToSlug(input.urlPath) };
}

/** Remove an endpoint (and its YAML) from the server. */
export async function deleteEndpointOnServer(http: HttpClient, urlPath: string): Promise<void> {
  await callTool(http, 'flapi_delete_endpoint', { path: urlPath });
}

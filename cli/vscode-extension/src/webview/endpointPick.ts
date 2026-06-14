export interface EndpointPickItem {
  label: string;
  description: string;
  config: any;
}

/**
 * Build QuickPick items for choosing among endpoints that share a SQL template.
 * Pure (no vscode dependency) so it can be unit-tested. Handles both hyphen-cased
 * (`url-path`, `template-source`) and camelCased (`urlPath`, `templateSource`) keys,
 * and MCP entities that have a name instead of a URL path.
 */
export function buildEndpointPickItems(configs: any[]): EndpointPickItem[] {
  return configs.map((cfg) => {
    const urlPath =
      cfg['url-path'] || cfg.urlPath || cfg.mcp_name || cfg.mcpName || '(unnamed)';
    const method = (cfg.method || '').toString().toUpperCase();
    const template = cfg['template-source'] || cfg.templateSource || '';
    return {
      label: method ? `${method} ${urlPath}` : `${urlPath}`,
      description: template,
      config: cfg,
    };
  });
}

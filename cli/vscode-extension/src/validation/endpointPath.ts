/**
 * The url-path an endpoint YAML defines, read line by line: endpoint files may carry
 * `{{include ...}}` directives, which are not YAML until the server expands them.
 */
export function endpointPathFromYamlText(text: string): string | null {
  const m = /^url-path:[ \t]*(.+)$/m.exec(text);
  if (!m) {
    return null;
  }
  const value = m[1].trim().replace(/\s+#.*$/, '').replace(/^['"]|['"]$/g, '');
  return value || null;
}

/**
 * Which names carry credentials, and the two ways to hide their values:
 *  - redactMap: `<redacted>`, for anything LOGGED or shown;
 *  - blankMap:  empty string, for anything PERSISTED - restoring the name with an
 *    empty value lets the UI show the row without ever sending "<redacted>".
 */
const SENSITIVE_NAME = /authorization|token|secret|password|passwd|api[-_]?key|cookie|credential|auth/i;

export function isSensitiveName(name: string): boolean {
  return SENSITIVE_NAME.test(name);
}

export function redactMap(values: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [name, value] of Object.entries(values ?? {})) {
    out[name] = isSensitiveName(name) ? '<redacted>' : value;
  }
  return out;
}

export function blankMap(values: Record<string, string> | undefined): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [name, value] of Object.entries(values ?? {})) {
    out[name] = isSensitiveName(name) ? '' : value;
  }
  return out;
}

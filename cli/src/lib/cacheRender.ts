/** One cache-audit row reduced to the columns `cache audit -o table` shows. */
export function cacheAuditRows(rows: unknown, withEndpoint = false): Record<string, unknown>[] {
  if (!Array.isArray(rows)) {
    return [];
  }
  return rows
    .filter((row): row is Record<string, unknown> => !!row && typeof row === 'object')
    .map((row) => {
      const out: Record<string, unknown> = {};
      if (withEndpoint) {
        out.endpoint = row.endpoint_path ?? '';
      }
      out.time = row.sync_started_at ?? row.sync_completed_at ?? '';
      out.mode = row.sync_type ?? '';
      out.status = row.status ?? '';
      out.duration_ms = row.duration_ms ?? '';
      out.rows = row.rows_affected ?? '';
      out.snapshot = row.snapshot_id ?? '';
      return out;
    });
}

/**
 * The lines `cache refresh` prints for a successful refresh: whatever of
 * mode / status / rows / duration / snapshot the server reported. An empty
 * body (a bare 200) yields none - never a stray `""`.
 */
export function describeRefreshResult(result: unknown): string[] {
  if (!result || typeof result !== 'object' || Array.isArray(result)) {
    return [];
  }
  const r = result as Record<string, unknown>;
  const pick = (...keys: string[]) =>
    keys.map((k) => r[k]).find((v) => v !== undefined && v !== null && v !== '');
  const lines: string[] = [];
  const mode = pick('mode', 'sync_type', 'refresh_mode');
  const status = pick('status');
  const rows = pick('rows', 'rows_affected', 'row_count');
  const duration = pick('duration_ms', 'durationMs');
  const snapshot = pick('snapshot', 'snapshot_id');
  if (mode !== undefined) lines.push(`Mode: ${String(mode)}`);
  if (status !== undefined) lines.push(`Status: ${String(status)}`);
  if (rows !== undefined) lines.push(`Rows: ${String(rows)}`);
  if (duration !== undefined) lines.push(`Duration: ${String(duration)} ms`);
  if (snapshot !== undefined) lines.push(`Snapshot: ${String(snapshot)}`);
  const message = pick('message');
  if (lines.length === 0 && message !== undefined) lines.push(String(message));
  return lines;
}

/** Query parameters for POST .../cache/refresh. `--force` is a deprecated alias of a normal refresh. */
export function refreshQuery(options: { full?: boolean; force?: boolean }): Record<string, string> | undefined {
  return options.full ? { full: 'true' } : undefined;
}

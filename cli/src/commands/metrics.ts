import type { Command } from 'commander';
import type { CliContext } from '../lib/types';
import { Console } from '../lib/console';
import { handleError } from '../lib/errors';
import { renderJson, renderTable } from '../lib/render';
import chalk from 'chalk';

/** One row of the `requests.series` block of GET /api/v1/_config/metrics. */
export interface RequestSeries {
  protocol: 'rest' | 'mcp';
  method?: string;
  route?: string;
  tool?: string;
  requests: number;
  failures?: number;
  /** 401/403 (REST) and role denials (MCP): refused callers, not failures. */
  rejected?: number;
  /** `503_cache`: cache not ready (warming/not built/failed), not counted in 5xx. */
  status?: Partial<Record<'2xx' | '3xx' | '4xx' | '5xx' | '503_cache', number>>;
  latency_ms: { avg: number; p50: number; p95: number; p99: number; max: number };
}

/** One row of the `caches` block. */
export interface CacheSeries {
  cache: string;
  state: string;
  endpoint?: string;
  refreshes?: number;
  failures?: number;
  last?: { mode?: string; status?: string; duration_ms?: number; rows_affected?: number; completed_at?: string };
  duration_ms?: { avg: number; max: number };
  data_age_seconds?: number;
}

const fmtMs = (v: number | undefined) => (v === undefined ? '-' : v < 10 ? v.toFixed(2) : v.toFixed(0));

/** Request series as table rows, slowest p95 first. */
export function requestRows(series: RequestSeries[]): Record<string, unknown>[] {
  return [...series]
    .sort((a, b) => b.latency_ms.p95 - a.latency_ms.p95)
    .map((s) => ({
      endpoint: s.protocol === 'mcp' ? `tool ${s.tool}` : `${s.method} ${s.route}`,
      requests: s.requests,
      errors:
        s.protocol === 'mcp'
          ? s.failures ?? 0
          : `${s.status?.['4xx'] ?? 0} 4xx / ${s.status?.['5xx'] ?? 0} 5xx` +
            (s.status?.['503_cache'] ? ` / ${s.status['503_cache']} cache-not-ready` : ''),
      rejected: s.rejected ?? 0,
      avg_ms: fmtMs(s.latency_ms.avg),
      // Estimates interpolated inside a histogram bucket (never above max),
      // hence `~`: they used to be the bucket's upper bound, shown as `≤`.
      p50_ms: `~${fmtMs(s.latency_ms.p50)}`,
      p95_ms: `~${fmtMs(s.latency_ms.p95)}`,
      p99_ms: `~${fmtMs(s.latency_ms.p99)}`,
      max_ms: fmtMs(s.latency_ms.max),
    }));
}

/** Cache series as table rows, slowest last refresh first. */
export function cacheRows(caches: CacheSeries[]): Record<string, unknown>[] {
  return [...caches]
    .sort((a, b) => (b.last?.duration_ms ?? -1) - (a.last?.duration_ms ?? -1))
    .map((c) => ({
      cache: c.cache,
      state: c.state,
      mode: c.last?.mode ?? '-',
      last_status: c.last?.status ?? 'never refreshed',
      last_ms: c.last?.duration_ms ?? '-',
      rows: c.last?.rows_affected ?? '-',
      avg_ms: c.duration_ms?.avg ?? '-',
      refreshes: `${c.refreshes ?? 0} (${c.failures ?? 0} failed)`,
      age_s: c.data_age_seconds ?? '-',
    }));
}

export function registerMetricsCommand(program: Command, ctx: CliContext) {
  program
    .command('metrics')
    .description(
      'Runtime metrics since server start: per-endpoint/tool requests, errors and latency, ' +
        '401/403/429 rejections, and per-cache refresh duration, rows and data age',
    )
    .option('-e, --endpoint <path>', 'Only this endpoint path (or MCP tool name)')
    .action(async (options: { endpoint?: string }) => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching runtime metrics...');
      }
      try {
        const response = await ctx.client.get('/api/v1/_config/metrics', {
          params: options.endpoint ? { endpoint: options.endpoint } : undefined,
        });
        spinner?.succeed(chalk.green('✓ Runtime metrics retrieved'));
        const data = response.data ?? {};

        if (ctx.config.output === 'json') {
          renderJson(data);
          return;
        }

        const requests = data.requests ?? {};
        const rejections = requests.rejections ?? {};
        Console.info(chalk.cyan('\n📈 Requests'));
        Console.info(chalk.gray('═'.repeat(60)));
        Console.info(
          `${requests.total ?? 0} requests, ${requests.server_errors ?? 0} server errors, ` +
            `${requests.cache_unavailable ?? 0} cache-not-ready 503s · ` +
            `rejected: ${rejections.unauthorized ?? 0} × 401, ${rejections.forbidden ?? 0} × 403, ` +
            `${rejections.rate_limited ?? 0} × 429`,
        );
        renderTable(requestRows(requests.series ?? []));
        Console.info(chalk.dim('Percentiles are histogram bucket upper bounds, capped at the observed max.'));

        Console.info(chalk.cyan('\n🗄️  Caches'));
        Console.info(chalk.gray('═'.repeat(60)));
        renderTable(cacheRows(data.caches ?? []));
      } catch (error) {
        spinner?.fail(chalk.red('✗ Failed to fetch runtime metrics'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

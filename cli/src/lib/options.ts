import type { FlapiiConfig } from './types';

export function applyOutputOverride(
  config: FlapiiConfig,
  override?: 'json' | 'table',
): FlapiiConfig {
  if (!override) return config;
  return {
    ...config,
    output: override,
  };
}


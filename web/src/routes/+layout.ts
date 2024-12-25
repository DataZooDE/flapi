import type { LayoutLoad } from './$types';
import { fetchConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export const load = (async () => {
  try {
    const config = await fetchConfig();
    if (!config) {
      throw new Error('Configuration data is null or undefined');
    }

    return {
      endpoints: config.endpoints || {},
      connections: config.flapi?.connections || {}
    };
  } catch (e) {
    console.error('Failed to load configuration:', {
      error: e,
      message: e instanceof Error ? e.message : 'Unknown error',
      stack: e instanceof Error ? e.stack : undefined,
      timestamp: new Date().toISOString()
    });

    throw error(500, 'Failed to load application configuration');
  }
}) satisfies LayoutLoad;

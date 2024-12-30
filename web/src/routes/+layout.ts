import type { LayoutLoad } from './$types';
import { getProjectConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export const load: LayoutLoad = async ({ fetch }) => {
  try {
    const config = await getProjectConfig(fetch);
    return {
      config
    };
  } catch (e) {
    console.error('Failed to load configuration:', e);
    throw error(500, {
      message: 'Failed to load application configuration'
    });
  }
};

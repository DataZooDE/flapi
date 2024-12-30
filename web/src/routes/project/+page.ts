import type { PageLoad } from './$types';
import { getProjectConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export const load: PageLoad = async ({ fetch }) => {
  try {
    const config = await getProjectConfig(fetch);
    return { config };
  } catch (e) {
    console.error('Failed to load project config:', e);
    throw error(500, 'Failed to load project configuration');
  }
}; 
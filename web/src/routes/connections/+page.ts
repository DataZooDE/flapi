import type { PageLoad } from './$types';
import { getProjectConfig } from '$lib/api';

export const load: PageLoad = async () => {
  const config = await getProjectConfig();
  return {
    connections: config.connections || {}
  };
}; 
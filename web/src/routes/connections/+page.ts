import type { PageLoad } from './$types';
import { fetchConfig } from '$lib/api';

export const load: PageLoad = async () => {
  const config = await fetchConfig();
  return {
    connections: Object.entries(config.flapi.connections).map(([name, config]) => ({
      name,
      ...config
    }))
  };
}; 
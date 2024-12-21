import type { PageLoad } from './$types';
import { fetchConfig } from '$lib/api';

export const load: PageLoad = async () => {
  const config = await fetchConfig();
  return {
    endpoints: Object.values(config.endpoints),
    flapi: config.flapi
  };
}; 
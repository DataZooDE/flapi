import type { PageLoad } from './$types';
import { fetchConfig } from '$lib/api';

export const load: PageLoad = async ({ params }) => {
  const config = await fetchConfig();
  const endpoint = config.endpoints[decodeURIComponent(params.path)];
  
  return {
    endpoint,
    flapi: config.flapi,
    isNew: !endpoint
  };
}; 
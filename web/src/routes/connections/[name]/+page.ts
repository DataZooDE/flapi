import type { PageLoad } from './$types';
import { fetchConfig } from '$lib/api';

export const load: PageLoad = async ({ params }) => {
  const config = await fetchConfig();
  const connection = config.flapi.connections[decodeURIComponent(params.name)];
  
  return {
    connection: connection ? { name: params.name, ...connection } : undefined,
    isNew: !connection
  };
}; 
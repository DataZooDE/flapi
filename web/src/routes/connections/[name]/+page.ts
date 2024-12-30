import type { PageLoad } from './$types';
import { getProjectConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export const load: PageLoad = async ({ params }) => {
  try {
    const config = await getProjectConfig();
    const connection = config.connections?.[params.name];
    
    if (!connection && params.name !== 'new') {
      throw error(404, 'Connection not found');
    }

    return {
      connection: connection ? { ...connection, name: params.name } : undefined,
      isNew: params.name === 'new'
    };
  } catch (e) {
    if (e instanceof Error) {
      throw error(500, e.message);
    }
    throw e;
  }
}; 
import type { PageLoad } from './$types';
import { getEndpoint, getEndpointTemplate, getCacheConfig, getCacheTemplate } from '$lib/api';

export const load: PageLoad = async ({ params, fetch }) => {
  const [endpoint, template, cacheConfig, cacheTemplate] = await Promise.all([
    getEndpoint(params.path, fetch),
    getEndpointTemplate(params.path, fetch).catch(() => ''),
    getCacheConfig(params.path, fetch).catch(() => null),
    getCacheTemplate(params.path, fetch).catch(() => '')
  ]);

  return { 
    endpoint,
    template,
    cacheConfig,
    cacheTemplate
  };
}; 
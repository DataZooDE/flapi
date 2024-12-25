import { error } from '@sveltejs/kit';
import type { PageLoad } from './$types';
import type { EndpointConfig } from '$lib/types';

export const load: PageLoad = async ({ params, fetch }) => {
  try {
    // Load endpoint configuration
    const endpointResponse = await fetch(`/api/endpoints/${params.path}`);
    if (!endpointResponse.ok) {
      throw new Error(`Failed to load endpoint: ${endpointResponse.statusText}`);
    }
    const endpoint: EndpointConfig = await endpointResponse.json();

    // Load SQL template content if templateSource is specified
    if (endpoint.templateSource) {
      const templateResponse = await fetch(`/api/templates/${endpoint.templateSource}`);
      if (templateResponse.ok) {
        endpoint.template = await templateResponse.text();
      } else {
        console.warn(`Failed to load SQL template: ${templateResponse.statusText}`);
      }
    }

    return {
      endpoint
    };
  } catch (e) {
    console.error('Error loading endpoint:', e);
    throw error(404, 'Endpoint not found');
  }
}; 
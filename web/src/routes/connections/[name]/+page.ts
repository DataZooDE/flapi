import type { PageLoad } from './$types';
import { fetchConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export const load = (async ({ params }) => {
  try {
    const config = await fetchConfig();
    if (!config) {
      throw new Error('Configuration data is null or undefined');
    }

    const name = decodeURIComponent(params.name);
    
    // For new connections, return empty data
    if (name === 'new') {
      return {
        connection: null,
        isNew: true
      };
    }

    // For existing connections, validate the data
    if (!config.flapi) {
      throw new Error('FLAPI configuration is missing');
    }
    if (!config.flapi.connections) {
      throw new Error('Connections data is missing from configuration');
    }

    const connection = config.flapi.connections[name];
    if (!connection) {
      // Log the 404 error
      console.error('Connection not found:', {
        name,
        availableConnections: Object.keys(config.flapi.connections),
        timestamp: new Date().toISOString()
      });

      throw error(404, `Connection "${name}" not found`);
    }

    return {
      connection,
      isNew: false
    };
  } catch (e) {
    // Log detailed error for debugging
    const errorDetails = {
      message: e instanceof Error ? e.message : 'Unknown error',
      params,
      stack: e instanceof Error ? e.stack : undefined,
      timestamp: new Date().toISOString()
    };
    
    console.error('Failed to load connection:', errorDetails);

    // If it's already a SvelteKit error (has status), rethrow it
    if (e instanceof Error && 'status' in e) throw e;

    // Create a detailed error message
    const errorMessage = [
      'Failed to load connection configuration.',
      `Error: ${errorDetails.message}`,
      `Name: ${params.name}`,
      `Time: ${errorDetails.timestamp}`,
      errorDetails.stack
    ].filter(Boolean).join('\n');

    // Throw a 500 error with the detailed message
    throw error(500, errorMessage);
  }
}) satisfies PageLoad; 
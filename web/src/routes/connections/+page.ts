import { fetchConfig } from '$lib/api';
import { error } from '@sveltejs/kit';

export async function load() {
  try {
    const config = await fetchConfig();
    if (!config) {
      throw new Error('Configuration data is null or undefined');
    }
    if (!config.flapi) {
      throw new Error('FLAPI configuration is missing');
    }
    if (!config.flapi.connections) {
      throw new Error('Connections data is missing from configuration');
    }
    return {
      connections: config.flapi.connections
    };
  } catch (e) {
    // Create a detailed error message for logging
    const errorDetails = {
      errorMessage: e instanceof Error ? e.message : 'An unknown error occurred',
      timestamp: new Date().toISOString(),
      stack: e instanceof Error ? e.stack : undefined,
      context: 'Connections page data loader'
    };
    
    // Log detailed error for debugging
    console.error('Failed to load connections:', {
      ...errorDetails,
      error: e
    });

    // Create an Error instance with detailed message
    const errorMessage = [
      'Failed to load connections configuration.',
      `Error: ${errorDetails.errorMessage}`,
      `Context: ${errorDetails.context}`,
      `Time: ${errorDetails.timestamp}`,
      errorDetails.stack ? `Stack: ${errorDetails.stack}` : ''
    ].filter(Boolean).join('\n');

    // Throw a user-friendly error
    throw error(500, errorMessage);
  }
}; 
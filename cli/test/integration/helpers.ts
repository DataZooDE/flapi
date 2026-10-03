import path from 'node:path';
import { execa } from 'execa';

export const cliPath = path.resolve('dist', 'index.js');

/** Run the built `flapii` against the real server started by global-setup. */
export async function flapii(args: string[], env: Record<string, string | undefined> = {}) {
  return await execa('node', [cliPath, ...args], {
    reject: false,
    env: {
      ...process.env,
      FLAPI_BASE_URL: process.env.FLAPI_BASE_URL,
      FLAPI_CONFIG: process.env.FLAPI_CONFIG,
      FLAPI_CONFIG_SERVICE_TOKEN: process.env.FLAPI_CONFIG_SERVICE_TOKEN,
      ...env,
    },
  });
}

export const TOKEN = process.env.FLAPI_CONFIG_SERVICE_TOKEN ?? '';

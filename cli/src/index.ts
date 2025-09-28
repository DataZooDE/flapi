import { Command } from 'commander';
import { Console } from './lib/console';
import { loadConfig } from './lib/config';
import { createApiClient } from './lib/http';
import { handleError } from './lib/errors';
import { registerPingCommand } from './commands/ping';
import { registerConfigCommands } from './commands/config';
import { registerProjectCommands } from './commands/project';
import { registerEndpointCommands } from './commands/endpoints';
import { registerMcpCommands } from './commands/mcp';
import { registerTemplateCommands } from './commands/templates';
import { registerCacheCommands } from './commands/cache';
import { registerSchemaCommands } from './commands/schema';
import type { CliContext, FlapiiConfig } from './lib/types';
import type { AxiosInstance } from 'axios';

export async function createCli(argv = process.argv) {
  const program = new Command();

  program
    .name('flapii')
    .description('CLI client for flapi ConfigService')
    .configureOutput({
      outputError: (str, write) => write(Console.color('red', str)),
    })
    .option('-c, --config <path>', 'Path to flapi.yaml configuration file')
    .option('-u, --base-url <url>', 'Base URL of the flapi server', process.env.FLAPI_BASE_URL)
    .option('-t, --auth-token <token>', 'Bearer token for authentication', process.env.FLAPI_TOKEN)
    .option('--timeout <seconds>', 'Request timeout in seconds', process.env.FLAPI_TIMEOUT)
    .option('--retries <count>', 'Number of retries for failed requests', process.env.FLAPI_RETRIES)
    .option('--insecure', 'Disable TLS/SSL certificate verification', false)
    .option('-o, --output <format>', 'Output format: json or table', 'json')
    .option('--json-style <style>', 'JSON key casing style: camel or hyphen', 'camel')
    .option('--debug-http', 'Enable HTTP request/response debugging', false)
    .option('-q, --quiet', 'Suppress non-error output', false)
    .option('-y, --yes', 'Assume yes for prompts', false);

  let cachedConfig: FlapiiConfig | undefined;
  let cachedClient: AxiosInstance | undefined;

  const ctx: CliContext = {
    get config() {
      if (!cachedConfig) {
        cachedConfig = loadConfig(program.opts());
      }
      return cachedConfig;
    },
    get client() {
      if (!cachedClient) {
        cachedClient = createApiClient(this.config);
      }
      return cachedClient;
    },
  };

  registerPingCommand(program, ctx);
  registerConfigCommands(program, ctx);
  registerProjectCommands(program, ctx);
  registerEndpointCommands(program, ctx);
  registerTemplateCommands(program, ctx);
  registerCacheCommands(program, ctx);
  registerSchemaCommands(program, ctx);
  registerMcpCommands(program, ctx);

  try {
    await program.parseAsync(argv);
  } catch (error) {
    handleError(error, program.opts());
    process.exitCode = 1;
  }
}

// Always run CLI when this module is executed
createCli();


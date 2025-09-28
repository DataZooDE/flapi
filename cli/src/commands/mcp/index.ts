import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { registerMcpToolsCommand } from './tools';
import { registerMcpResourcesCommand } from './resources';
import { registerMcpPromptsCommand } from './prompts';

export function registerMcpCommands(program: Command, ctx: CliContext) {
  const mcp = program.command('mcp').description('MCP (Model Context Protocol) commands');

  registerMcpToolsCommand(mcp, ctx);
  registerMcpResourcesCommand(mcp, ctx);
  registerMcpPromptsCommand(mcp, ctx);
}


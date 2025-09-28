import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { registerConfigCommand } from './show';

export function registerConfigCommands(program: Command, ctx: CliContext) {
  registerConfigCommand(program, ctx);
}


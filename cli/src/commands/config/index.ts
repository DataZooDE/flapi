import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { registerConfigCommand } from './show';
import { registerValidateCommand } from './validate';
import { registerLogLevelCommands } from './log-level';
import { registerEnvironmentCommand } from './environment';
import { registerFilesystemCommand } from './filesystem';

export function registerConfigCommands(program: Command, ctx: CliContext) {
  const configCmd = registerConfigCommand(program, ctx);
  registerValidateCommand(configCmd, ctx);
  registerLogLevelCommands(configCmd, ctx);
  registerEnvironmentCommand(configCmd, ctx);
  registerFilesystemCommand(configCmd, ctx);
}

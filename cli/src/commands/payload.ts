import { Command } from 'commander';
import type { CliContext } from '../lib/types';
import { loadPayload } from '../lib/payload';

export function withPayloadOptions(cmd: Command) {
  return cmd
    .option('-f, --file <path>', 'Path to JSON/YAML file containing payload')
    .option('--stdin', 'Read payload from stdin');
}

export async function resolvePayload(options: { file?: string; stdin?: boolean }) {
  return await loadPayload({ file: options.file, stdin: options.stdin });
}


import fs from 'node:fs';
import path from 'node:path';
import { load as loadYaml } from 'js-yaml';

export interface PayloadSource {
  file?: string;
  stdin?: boolean;
}

export async function loadPayload(source: PayloadSource): Promise<unknown> {
  if (source.file && source.stdin) {
    throw new Error('Cannot specify both --file and --stdin');
  }

  let raw: string;

  if (source.file) {
    raw = fs.readFileSync(path.resolve(source.file), 'utf-8');
  } else if (source.stdin) {
    raw = await readFromStdin();
  } else {
    throw new Error('Must provide either --file or --stdin');
  }

  return parsePayload(raw, source.file);
}

export function parsePayload(raw: string, filePath?: string): unknown {
  const trimmed = raw.trim();
  if (!trimmed) {
    throw new Error('Payload is empty');
  }

  const ext = filePath ? path.extname(filePath).toLowerCase() : undefined;

  if (ext === '.json') {
    return JSON.parse(trimmed);
  }

  if (ext === '.yaml' || ext === '.yml') {
    return loadYaml(trimmed);
  }

  if (looksLikeJson(trimmed)) {
    return JSON.parse(trimmed);
  }

  return loadYaml(trimmed);
}

function looksLikeJson(input: string): boolean {
  return (input.startsWith('{') && input.endsWith('}')) || (input.startsWith('[') && input.endsWith(']'));
}

async function readFromStdin(): Promise<string> {
  const chunks: string[] = [];

  if (process.stdin.isTTY) {
    throw new Error('No stdin data available');
  }

  return await new Promise<string>((resolve, reject) => {
    process.stdin.setEncoding('utf-8');
    process.stdin.on('data', (chunk) => chunks.push(chunk));
    process.stdin.once('end', () => resolve(chunks.join('')));
    process.stdin.once('error', reject);
  });
}

import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';

const cliPath = path.resolve('dist', 'index.js');

describe('cli config command (integration)', () => {
  it('prints configuration in table format', async () => {
    const result = await execa('node', [cliPath, 'config', '--output', 'table']);

    expect(result.exitCode).toBe(0);
    expect(result.stdout).toContain('Setting');
  });
});


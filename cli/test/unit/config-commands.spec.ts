import { describe, it, expect, beforeEach, vi } from 'vitest';
import { Command } from 'commander';
import { registerConfigCommands } from '../../src/commands/config';
import type { CliContext, FlapiiConfig } from '../../src/lib/types';

const spinner = { succeed: vi.fn(), fail: vi.fn(), stop: vi.fn(), text: '' };

vi.mock('../../src/lib/console', () => ({
  Console: {
    spinner: () => spinner,
    info: vi.fn(),
    warn: vi.fn(),
    color: vi.fn((_, str) => str),
  },
}));

vi.mock('../../src/lib/render', () => ({
  renderConfig: vi.fn(),
  renderJson: vi.fn(),
}));

describe('config commands (env/filesystem)', () => {
  const mockClient = {
    get: vi.fn(),
    put: vi.fn(),
  };

  const config: FlapiiConfig = {
    baseUrl: 'http://localhost:8080',
    timeout: 10,
    retries: 2,
    verifyTls: true,
    output: 'json',
    jsonStyle: 'camel',
    debugHttp: false,
    quiet: false,
    yes: false,
  };

  const ctx: CliContext = {
    get config() {
      return config;
    },
    get client() {
      return mockClient as any;
    },
  };

  beforeEach(() => {
    mockClient.get.mockReset();
    mockClient.put.mockReset();
  });

  it('fetches environment variables via config env command', async () => {
    mockClient.get.mockResolvedValue({ data: { variables: [] } });
    const program = new Command();
    program.exitOverride();
    registerConfigCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'config', 'env']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/environment-variables');
  });

  it('fetches filesystem info via config filesystem command', async () => {
    mockClient.get.mockResolvedValue({ data: { base_path: '/tmp', tree: [] } });
    const program = new Command();
    program.exitOverride();
    registerConfigCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'config', 'filesystem']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/filesystem');
  });
});

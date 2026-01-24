import { describe, it, expect, beforeEach, vi } from 'vitest';
import { Command } from 'commander';
import { registerCacheCommands } from '../../src/commands/cache';
import type { CliContext, FlapiiConfig } from '../../src/lib/types';

const spinner = { succeed: vi.fn(), fail: vi.fn(), stop: vi.fn(), text: '' };

vi.mock('../../src/lib/console', () => ({
  Console: {
    spinner: () => spinner,
    info: vi.fn(),
    warn: vi.fn(),
    success: vi.fn(),
    color: vi.fn((_, str) => str),
  },
}));

vi.mock('../../src/lib/render', () => ({
  renderJson: vi.fn(),
  renderCacheTable: vi.fn(),
}));

describe('cache commands', () => {
  const mockClient = {
    get: vi.fn(),
    post: vi.fn(),
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
    mockClient.post.mockReset();
    mockClient.put.mockReset();
  });

  it('retrieves cache template value from response wrapper', async () => {
    mockClient.get.mockResolvedValue({ data: { template: 'select 1;' } });
    const program = new Command();
    program.exitOverride();
    registerCacheCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'cache', 'template', '/foo']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/endpoints/foo/cache/template');
  });

  it('invokes cache gc endpoint', async () => {
    mockClient.post.mockResolvedValue({ data: {} });
    const program = new Command();
    program.exitOverride();
    registerCacheCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'cache', 'gc', '/foo']);

    expect(mockClient.post).toHaveBeenCalledWith('/api/v1/_config/endpoints/foo/cache/gc');
  });
  it('errors if cursor flag is incomplete', async () => {
    const program = new Command();
    program.exitOverride();
    registerCacheCommands(program, ctx);

    await expect(
      program.parseAsync(['node', 'test', 'cache', 'update', '/foo', '--cursor-column', 'updated_at'])
    ).rejects.toThrow();
    expect(mockClient.put).not.toHaveBeenCalled();
  });
});

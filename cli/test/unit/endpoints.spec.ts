import { describe, it, expect, vi, beforeEach } from 'vitest';
import { Command } from 'commander';
import { registerEndpointCommands } from '../../src/commands/endpoints';
import { buildEndpointUrl } from '../../src/lib/url';
import type { CliContext, FlapiiConfig } from '../../src/lib/types';

const spinner = { succeed: vi.fn(), fail: vi.fn(), stop: vi.fn() };

vi.mock('../../src/lib/console', () => ({
  Console: {
    spinner: () => spinner,
    info: vi.fn(),
    warn: vi.fn(),
    color: vi.fn((_, str) => str),
  },
}));

vi.mock('../../src/lib/render', () => ({
  renderJson: vi.fn(),
  renderEndpointsTable: vi.fn(),
  renderEndpointTable: vi.fn(),
}));

describe('endpoints command', () => {
  const mockClient = {
    get: vi.fn(),
    post: vi.fn(),
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
    spinner.succeed.mockReset();
    spinner.fail.mockReset();
    spinner.stop.mockReset();
  });

  it('fetches endpoint list', async () => {
    mockClient.get.mockResolvedValue({ data: {} });
    const program = new Command().exitOverride();
    registerEndpointCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'endpoints', 'list']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/endpoints');
  });

  it('fetches single endpoint', async () => {
    mockClient.get.mockResolvedValue({ data: { path: '/foo' } });
    const program = new Command().exitOverride();
    registerEndpointCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'endpoints', 'get', '/foo']);

    expect(mockClient.get).toHaveBeenCalledWith(buildEndpointUrl('/foo'));
  });

  it('fetches endpoint parameters', async () => {
    mockClient.get.mockResolvedValue({ data: { parameters: [] } });
    const program = new Command().exitOverride();
    registerEndpointCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'endpoints', 'parameters', '/foo']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/endpoints/foo/parameters');
  });
});

import { describe, it, expect, beforeEach, vi } from 'vitest';
import { Command } from 'commander';
import { registerTemplateCommands } from '../../src/commands/templates';
import type { CliContext, FlapiiConfig } from '../../src/lib/types';

const spinner = { succeed: vi.fn(), fail: vi.fn(), stop: vi.fn() };

vi.mock('../../src/lib/console', () => ({
  Console: {
    spinner: () => spinner,
    info: vi.fn(),
    success: vi.fn(),
    color: vi.fn((_, str) => str),
  },
}));

vi.mock('../../src/lib/render', () => ({
  renderJson: vi.fn(),
  renderTemplatesTable: vi.fn(),
}));

describe('template commands', () => {
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

  it('fetches template content using template field', async () => {
    mockClient.get.mockResolvedValue({ data: { template: 'select 1;' } });
    const program = new Command();
    program.exitOverride();
    registerTemplateCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'templates', 'get', '/foo']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/endpoints/foo/template');
  });

  it('tests template execution with success payload', async () => {
    mockClient.post.mockResolvedValue({ data: { success: true, rows: [{ id: 1 }], columns: ['id'] } });
    const program = new Command();
    program.exitOverride();
    registerTemplateCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'templates', 'test', '/foo', '-p', '{"id":1}']);

    expect(mockClient.post).toHaveBeenCalledWith('/api/v1/_config/endpoints/foo/template/test', {
      parameters: { id: 1 },
    });
  });

  it('finds endpoints by template file', async () => {
    mockClient.post.mockResolvedValue({ data: { endpoints: [] } });
    const program = new Command().exitOverride();
    registerTemplateCommands(program, ctx);

    await program.parseAsync(['node', 'test', 'templates', 'find', '--template', 'foo.sql']);

    expect(mockClient.post).toHaveBeenCalledWith('/api/v1/_config/endpoints/by-template', {
      template_path: 'foo.sql',
    });
  });
});

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { Command } from 'commander';
import { registerMcpCommands } from '../../src/commands/mcp';

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
  renderMcpToolsTable: vi.fn(),
  renderMcpResourcesTable: vi.fn(),
  renderMcpPromptsTable: vi.fn(),
}));

const mockClient = {
  get: vi.fn(),
};

const ctx = {
  get config() {
    return {
      output: 'json',
      jsonStyle: 'camel',
    };
  },
  get client() {
    return mockClient as any;
  },
};

describe('mcp commands', () => {
  beforeEach(() => {
    mockClient.get.mockReset();
    spinner.succeed.mockReset();
    spinner.fail.mockReset();
    spinner.stop.mockReset();
  });

  it('lists MCP tools', async () => {
    mockClient.get.mockResolvedValue({ data: { '/path': { 'mcp-tool': { name: 'tool' } } } });
    const program = new Command();
    registerMcpCommands(program, ctx as any);

    await program.parseAsync(['node', 'test', 'mcp', 'tools', 'list']);

    expect(mockClient.get).toHaveBeenCalledWith('/api/v1/_config/endpoints');
  });

  it('gets MCP resource', async () => {
    mockClient.get.mockResolvedValue({ data: { '/resource': { 'mcp-resource': { name: 'res' } } } });
    const program = new Command();
    registerMcpCommands(program, ctx as any);

    await program.parseAsync(['node', 'test', 'mcp', 'resources', 'get', 'res']);

    expect(mockClient.get).toHaveBeenCalled();
  });

  it('gets MCP prompt', async () => {
    mockClient.get.mockResolvedValue({ data: { '/prompt': { 'mcp-prompt': { name: 'prompt' } } } });
    const program = new Command();
    registerMcpCommands(program, ctx as any);

    await program.parseAsync(['node', 'test', 'mcp', 'prompts', 'get', 'prompt']);

    expect(mockClient.get).toHaveBeenCalled();
  });
});


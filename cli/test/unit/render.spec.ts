import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  renderConfig,
  renderJson,
  renderEndpointTable,
  renderEndpointsTable,
  renderMcpToolsTable,
  renderMcpResourcesTable,
  renderMcpPromptsTable,
} from '../../src/lib/render';
import type { FlapiiConfig } from '../../src/lib/types';

const mockInfo = vi.fn();

vi.mock('../../src/lib/console', () => ({
  Console: {
    info: (...args: unknown[]) => mockInfo(...args),
    spinner: vi.fn(),
    success: vi.fn(),
    warn: vi.fn(),
    error: vi.fn(),
    color: vi.fn((_, str: string) => str),
  },
}));

describe('render.ts', () => {
  beforeEach(() => {
    mockInfo.mockReset();
  });

  it('renders config as JSON when output is json', () => {
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

    renderConfig(config);
    expect(mockInfo).toHaveBeenCalledWith(JSON.stringify(config, null, 2));
  });

  it('renders config as table when output is table', () => {
    const config: FlapiiConfig = {
      baseUrl: 'http://localhost:8080',
      timeout: 10,
      retries: 2,
      verifyTls: true,
      output: 'table',
      jsonStyle: 'camel',
      debugHttp: false,
      quiet: false,
      yes: false,
    };

    renderConfig(config);
    expect(mockInfo).toHaveBeenCalledTimes(3); // header + divider + table
    const headerCall = mockInfo.mock.calls[0][0] as string;
    const tableCall = mockInfo.mock.calls[2][0] as string;
    expect(headerCall).toContain('📋 Current Configuration');
    expect(tableCall).toContain('Setting');
    expect(tableCall).toContain('Value');
  });

  it('applies camel style to JSON output', () => {
    renderJson({ 'foo-bar': { 'baz-qux': 1 } }, 'camel');
    expect(mockInfo).toHaveBeenCalledWith(JSON.stringify({ fooBar: { bazQux: 1 } }, null, 2));
  });

  it('applies hyphen style to JSON output', () => {
    renderJson({ fooBar: { bazQux: 1 } }, 'hyphen');
    expect(mockInfo).toHaveBeenCalledWith(JSON.stringify({ 'foo-bar': { 'baz-qux': 1 } }, null, 2));
  });

  it('renders endpoints table', () => {
    renderEndpointsTable({ '/foo': { method: 'get' } });
    expect(mockInfo).toHaveBeenCalled();
  });

  it('renders endpoint detail table', () => {
    renderEndpointTable({ method: 'get', url: '/foo' });
    expect(mockInfo).toHaveBeenCalled();
  });

  it('renders MCP tools table', () => {
    renderMcpToolsTable([
      { path: '/tool', config: { 'mcp-tool': { name: 'tool', description: 'desc' } } },
    ]);
    expect(mockInfo).toHaveBeenCalled();
  });

  it('renders MCP resources table', () => {
    renderMcpResourcesTable([
      { path: '/resource', config: { 'mcp-resource': { name: 'resource', description: 'desc' } } },
    ]);
    expect(mockInfo).toHaveBeenCalled();
  });

  it('renders MCP prompts table', () => {
    renderMcpPromptsTable([
      { path: '/prompt', config: { 'mcp-prompt': { name: 'prompt', description: 'desc' } } },
    ]);
    expect(mockInfo).toHaveBeenCalled();
  });
});


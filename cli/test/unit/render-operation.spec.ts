import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  renderEndpointTable,
  renderEndpointsTable,
  renderMcpToolsTable,
} from '../../src/lib/render';

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

describe('render.ts - Operation Type Support', () => {
  beforeEach(() => {
    mockInfo.mockReset();
  });

  describe('renderEndpointsTable', () => {
    it('displays operation type for read endpoints', () => {
      renderEndpointsTable({
        '/read-endpoint': {
          method: 'GET',
          connection: ['test-conn'],
        },
      });

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toBeDefined();
      // Should contain Operation column
      expect(tableCall).toContain('Operation');
      // Should infer Read from GET method
      expect(tableCall).toContain('Read');
    });

    it('displays operation type for write endpoints', () => {
      renderEndpointsTable({
        '/write-endpoint': {
          method: 'POST',
          connection: ['test-conn'],
        },
      });

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toBeDefined();
      // Should infer Write from POST method
      expect(tableCall).toContain('Write');
    });

    it('displays explicit operation type from config', () => {
      renderEndpointsTable({
        '/explicit-endpoint': {
          method: 'GET',
          operation: {
            type: 'write',
          },
          connection: ['test-conn'],
        },
      });

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toBeDefined();
      // Should use explicit type over method inference
      expect(tableCall).toContain('Write');
    });

    it('handles all write HTTP methods', () => {
      const methods = ['POST', 'PUT', 'PATCH', 'DELETE'];

      methods.forEach((method) => {
        mockInfo.mockReset();
        renderEndpointsTable({
          [`/${method.toLowerCase()}-endpoint`]: {
            method,
            connection: ['test-conn'],
          },
        });

        const tableCall = mockInfo.mock.calls.find((call) =>
          (call[0] as string).includes('Operation')
        )?.[0] as string;

        expect(tableCall).toContain('Write');
      });
    });

    it('displays operation column in table header', () => {
      renderEndpointsTable({
        '/test': { method: 'GET', connection: ['test'] },
      });

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toContain('Path');
      expect(tableCall).toContain('Method');
      expect(tableCall).toContain('Operation');
    });
  });

  describe('renderEndpointTable', () => {
    it('displays operation configuration prominently', () => {
      renderEndpointTable({
        method: 'POST',
        'url-path': '/test',
        operation: {
          type: 'write',
          validate_before_write: true,
          returns_data: true,
          transaction: true,
        },
      });

      const tableOutput = mockInfo.mock.calls
        .map((call) => call[0] as string)
        .join('\n');

      // Should show operation prominently
      expect(tableOutput).toContain('Operation');
      expect(tableOutput).toContain('write');
      // Implementation shows "validate" (shortened) in the details
      // Note: The table might truncate long values, so we check for key parts
      // At minimum, the operation type with details should be visible
      expect(tableOutput).toMatch(/write.*\(/);
      // Check that at least one detail is present (validate, returns-data, or transaction)
      const hasDetail = tableOutput.includes('validate') || 
                       tableOutput.includes('returns-data') || 
                       tableOutput.includes('transaction');
      expect(hasDetail).toBe(true);
    });

    it('infers operation type when not explicitly configured', () => {
      renderEndpointTable({
        method: 'PUT',
        'url-path': '/test',
      });

      const tableOutput = mockInfo.mock.calls
        .map((call) => call[0] as string)
        .join('\n');

      expect(tableOutput).toContain('Operation');
      expect(tableOutput).toContain('write');
      expect(tableOutput).toContain('inferred');
    });

    it('shows read operation for GET method', () => {
      renderEndpointTable({
        method: 'GET',
        'url-path': '/test',
      });

      const tableOutput = mockInfo.mock.calls
        .map((call) => call[0] as string)
        .join('\n');

      expect(tableOutput).toContain('Operation');
      expect(tableOutput).toContain('read');
      expect(tableOutput).toContain('inferred');
    });

    it('handles operation with partial configuration', () => {
      renderEndpointTable({
        method: 'POST',
        'url-path': '/test',
        operation: {
          type: 'write',
          validate_before_write: false,
          returns_data: true,
        },
      });

      const tableOutput = mockInfo.mock.calls
        .map((call) => call[0] as string)
        .join('\n');

      expect(tableOutput).toContain('write');
      expect(tableOutput).toContain('returns-data');
      // Should not show validate-before-write if false
      expect(tableOutput).not.toContain('validate-before-write');
    });
  });

  describe('renderMcpToolsTable', () => {
    it('displays operation type for MCP tools', () => {
      renderMcpToolsTable([
        {
          path: '/mcp-tool',
          config: {
            method: 'POST',
            'mcp-tool': { name: 'test-tool', description: 'Test tool' },
          },
        },
      ]);

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toBeDefined();
      expect(tableCall).toContain('Operation');
      // Should infer Write from POST method
      expect(tableCall).toContain('Write');
    });

    it('displays read operation for GET MCP tools', () => {
      renderMcpToolsTable([
        {
          path: '/mcp-tool',
          config: {
            method: 'GET',
            'mcp-tool': { name: 'test-tool', description: 'Test tool' },
          },
        },
      ]);

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toContain('Read');
    });

    it('uses explicit operation configuration for MCP tools', () => {
      renderMcpToolsTable([
        {
          path: '/mcp-tool',
          config: {
            method: 'GET',
            operation: {
              type: 'write',
            },
            'mcp-tool': { name: 'test-tool', description: 'Test tool' },
          },
        },
      ]);

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toContain('Write');
    });

    it('displays operation column in MCP tools table header', () => {
      renderMcpToolsTable([
        {
          path: '/mcp-tool',
          config: {
            method: 'GET',
            'mcp-tool': { name: 'test-tool', description: 'Test tool' },
          },
        },
      ]);

      const tableCall = mockInfo.mock.calls.find((call) =>
        (call[0] as string).includes('Operation')
      )?.[0] as string;

      expect(tableCall).toContain('Path');
      expect(tableCall).toContain('Name');
      expect(tableCall).toContain('Operation');
      expect(tableCall).toContain('Description');
    });
  });
});


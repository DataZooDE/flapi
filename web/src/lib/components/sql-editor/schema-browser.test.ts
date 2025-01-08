import { render, fireEvent } from '@testing-library/svelte';
import SchemaBrowser from './schema-browser.svelte';
import { schemaStore } from '../../stores/schema';
import type { SchemaInfo, SchemaState } from '../../types';
import { describe, it, expect, vi, beforeEach } from 'vitest';

describe('SchemaBrowser', () => {
  const mockSchema: SchemaInfo = {
    public: {
      tables: {
        users: {
          is_view: false,
          columns: {
            id: { type: 'INTEGER', nullable: false },
            name: { type: 'TEXT', nullable: true }
          }
        }
      }
    }
  };

  beforeEach(() => {
    vi.spyOn(schemaStore, 'load').mockImplementation(async () => {});
    vi.spyOn(schemaStore, 'subscribe').mockImplementation((callback) => {
      const state: SchemaState = { data: mockSchema, isLoading: false, error: null };
      callback(state);
      return () => {};
    });
  });

  it('renders schema information', () => {
    const onTableClick = vi.fn();
    const { getByText } = render(SchemaBrowser, { props: { onTableClick } });

    expect(getByText('public')).toBeTruthy();
    expect(getByText('users')).toBeTruthy();
    expect(getByText('id: INTEGER')).toBeTruthy();
    expect(getByText('name: TEXT?')).toBeTruthy();
  });

  it('calls onTableClick with correct parameters', async () => {
    const onTableClick = vi.fn();
    const { getByText } = render(SchemaBrowser, { props: { onTableClick } });

    await fireEvent.click(getByText('users'));
    expect(onTableClick).toHaveBeenCalledWith('users', 'public');
  });
}); 
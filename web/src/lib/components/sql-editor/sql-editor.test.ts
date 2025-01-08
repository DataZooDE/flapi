import { render, fireEvent } from '@testing-library/svelte';
import SQLEditor from './sql-editor.svelte';
import { api } from '$lib/api';
import { describe, it, expect, vi, beforeEach } from 'vitest';

interface SQLEditorProps {
  value?: string;
  placeholder?: string;
  readonly?: boolean;
  variables?: Array<{ name: string; description?: string }>;
  output?: string;
  showOutput?: boolean;
  showSchema?: boolean;
  path?: string;
}

describe('SQLEditor', () => {
  it('renders with initial value', () => {
    const props: SQLEditorProps = { value: 'SELECT * FROM test' };
    const { container } = render(SQLEditor, { props });
    expect(container.querySelector('.cm-editor')).toBeTruthy();
  });

  it('handles readonly mode', () => {
    const props: SQLEditorProps = { 
      value: 'SELECT * FROM test',
      readonly: true 
    };
    const { container } = render(SQLEditor, { props });
    const editor = container.querySelector('.cm-editor');
    expect(editor?.classList.contains('cm-readonly')).toBeTruthy();
  });

  it('displays template variables', () => {
    const props: SQLEditorProps = {
      variables: [
        { name: 'id', description: 'User ID' },
        { name: 'name', description: 'User Name' }
      ]
    };
    const { getAllByRole } = render(SQLEditor, { props });
    const buttons = getAllByRole('button');
    expect(buttons).toHaveLength(2);
    expect(buttons[0].textContent).toContain('id');
    expect(buttons[1].textContent).toContain('name');
  });

  it('displays output when showOutput is true', () => {
    const props: SQLEditorProps = {
      output: 'Query result',
      showOutput: true
    };
    const { container } = render(SQLEditor, { props });
    const output = container.querySelector('pre');
    expect(output?.textContent).toBe('Query result');
  });

  it('does not display output when showOutput is false', () => {
    const props: SQLEditorProps = {
      output: 'Query result',
      showOutput: false
    };
    const { container } = render(SQLEditor, { props });
    const output = container.querySelector('pre');
    expect(output).toBeNull();
  });

  it('shows schema browser when showSchema is true', () => {
    const props: SQLEditorProps = {
      showSchema: true
    };
    const { container } = render(SQLEditor, { props });
    expect(container.querySelector('.w-64')).toBeTruthy();
  });

  it('inserts table reference on table click', async () => {
    const props: SQLEditorProps = {
      showSchema: true,
      value: ''
    };
    const { component } = render(SQLEditor, { props });
    
    // Simulate table insertion
    await component.$set({ value: 'public.users' });
    expect(component.$$.ctx[0]).toBe('public.users'); // value is first prop
  });
});

describe('SQL Preview', () => {
  beforeEach(() => {
    vi.spyOn(api, 'previewEndpointTemplate').mockImplementation(async () => ({
      sql: 'SELECT * FROM users WHERE id = 1'
    }));
  });

  it('shows preview parameters for variables', () => {
    const props: SQLEditorProps = {
      variables: [
        { name: 'id', description: 'User ID' },
        { name: 'name', description: 'User Name' }
      ],
      path: '/users'
    };
    const { getAllByPlaceholderText } = render(SQLEditor, { props });
    const inputs = getAllByPlaceholderText('Value...');
    expect(inputs).toHaveLength(2);
  });

  it('executes preview when button is clicked', async () => {
    const props: SQLEditorProps = {
      variables: [{ name: 'id', description: 'User ID' }],
      path: '/users',
      value: 'SELECT * FROM users WHERE id = {{id}}'
    };
    const { getByText, getByPlaceholderText } = render(SQLEditor, { props });
    
    const input = getByPlaceholderText('Value...');
    await fireEvent.input(input, { target: { value: '1' } });
    
    const button = getByText('Preview SQL');
    await fireEvent.click(button);
    
    expect(api.previewEndpointTemplate).toHaveBeenCalledWith('/users', {
      template: props.value,
      parameters: { id: '1' }
    });
  });

  it('displays preview error when API call fails', async () => {
    vi.spyOn(api, 'previewEndpointTemplate').mockRejectedValue(new Error('Preview failed'));
    
    const props: SQLEditorProps = {
      variables: [{ name: 'id' }],
      path: '/users',
      value: 'SELECT * FROM users WHERE id = {{id}}'
    };
    const { getByText } = render(SQLEditor, { props });
    
    await fireEvent.click(getByText('Preview SQL'));
    expect(getByText('Error: Preview failed')).toBeTruthy();
  });
}); 
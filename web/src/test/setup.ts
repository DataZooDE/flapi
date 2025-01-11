import '@testing-library/jest-dom';
import { vi } from 'vitest';
import { cleanup } from '@testing-library/svelte';

// Helper function to handle Svelte slots
function renderSlotContent(slots?: { default?: Array<any> }) {
  if (!slots?.default) return '';
  return slots.default
    .map(node => {
      if (typeof node === 'string') return node;
      if (node.text) return node.text;
      // Skip icon components
      if (node.$$render) return '';
      return '';
    })
    .join(' ')
    .trim();
}

// Helper to handle Svelte events
function mapSvelteEvents(props: Record<string, any>, element: HTMLElement) {
  Object.entries(props).forEach(([key, value]) => {
    if (key.startsWith('on:')) {
      const eventName = key.slice(3); // Remove 'on:'
      element[`on${eventName}`] = value;
    }
  });
}

// Mock UI components globally
vi.mock('$lib/components/ui/input', () => ({
  Input: vi.fn().mockImplementation((props) => ({
    $$render: () => {
      const input = document.createElement('input');
      input.id = props.id;
      input.value = props.value || '';
      input.placeholder = props.placeholder;
      input.type = 'text';
      input.className = props.class;
      
      // Handle Svelte events
      mapSvelteEvents(props, input);
      
      return input;
    }
  }))
}));

vi.mock('$lib/components/ui/button', () => ({
  Button: vi.fn().mockImplementation((props) => ({
    $$render: () => {
      const button = document.createElement('button');
      const slotContent = renderSlotContent(props.$$slots);
      button.textContent = slotContent || props.title || '';
      button.type = 'button';
      button.className = props.class || '';
      button.title = props.title || '';
      button.setAttribute('aria-label', props.title || slotContent || '');
      
      // Handle Svelte events
      mapSvelteEvents(props, button);
      
      return button;
    }
  }))
}));

vi.mock('$lib/components/ui/label', () => ({
  Label: vi.fn().mockImplementation((props) => ({
    $$render: () => {
      const label = document.createElement('label');
      label.htmlFor = props.for || '';
      label.textContent = renderSlotContent(props.$$slots);
      return label;
    }
  }))
}));

// Mock select element
vi.mock('$lib/components/ui/select', () => ({
  Select: vi.fn().mockImplementation((props) => ({
    $$render: () => {
      const select = document.createElement('select');
      select.id = props.id;
      select.value = props.value || '';
      select.className = props.class;
      
      // Handle Svelte events
      mapSvelteEvents(props, select);
      
      return select;
    }
  }))
}));

// Mock Lucide icons to return empty spans
vi.mock('lucide-svelte', () => ({
  Plus: vi.fn().mockImplementation(() => ({
    $$render: () => {
      const span = document.createElement('span');
      span.className = 'lucide-icon plus';
      span.setAttribute('aria-hidden', 'true');
      return span;
    }
  })),
  Trash: vi.fn().mockImplementation(() => ({
    $$render: () => {
      const span = document.createElement('span');
      span.className = 'lucide-icon trash';
      span.setAttribute('aria-hidden', 'true');
      return span;
    }
  }))
}));

// Clean up after each test
afterEach(() => {
  cleanup();
  vi.clearAllMocks();
}); 
/**
 * Shared Parameter Editor Component
 * 
 * Provides reusable HTML, CSS, and JavaScript for parameter editing
 * Used by both EndpointTesterPanel and SqlTemplateTesterPanel
 */

export interface ParameterDefinition {
  name: string;
  in: string;
  description?: string;
  required: boolean;
  default?: string;
  validators?: Array<{
    type: string;
    min?: number;
    max?: number;
    regex?: string;
    allowedValues?: string[];
  }>;
}

export interface ParameterEditorOptions {
  /** Whether to show the "Load Defaults" button */
  showLoadDefaults?: boolean;
  /** Whether to show parameter source dropdown (query, path, header, body) */
  showParameterSource?: boolean;
  /** Placeholder for parameter name input */
  namePlaceholder?: string;
  /** Placeholder for parameter value input */
  valuePlaceholder?: string;
  /** Available parameter names for autocomplete */
  availableParams?: ParameterDefinition[];
}

/**
 * Generate CSS for parameter editor
 */
export function getParameterEditorCSS(): string {
  return `
    .param-editor {
      padding: 16px;
      border-bottom: 1px solid var(--vscode-panel-border);
    }

    .param-editor h3 {
      margin: 0 0 12px 0;
      font-size: 14px;
      font-weight: 600;
    }

    .param-list {
      display: flex;
      flex-direction: column;
      gap: 8px;
      margin-bottom: 12px;
    }

    .param-row {
      display: flex;
      gap: 8px;
      align-items: center;
    }

    .param-row input[type="text"] {
      flex: 1;
      padding: 6px 8px;
      background: var(--vscode-input-background);
      color: var(--vscode-input-foreground);
      border: 1px solid var(--vscode-input-border);
      border-radius: 2px;
      font-family: var(--vscode-editor-font-family);
      font-size: 13px;
    }

    .param-row input[list] {
      cursor: text;
    }

    .param-row select {
      padding: 6px 8px;
      background: var(--vscode-input-background);
      color: var(--vscode-input-foreground);
      border: 1px solid var(--vscode-input-border);
      border-radius: 2px;
      font-size: 13px;
      cursor: pointer;
    }

    .param-row button {
      padding: 4px 8px;
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
      border: none;
      border-radius: 2px;
      cursor: pointer;
      font-size: 12px;
      white-space: nowrap;
    }

    .param-row button:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }

    .add-param-btn {
      padding: 6px 12px;
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
      border: none;
      border-radius: 2px;
      cursor: pointer;
      font-size: 12px;
    }

    .add-param-btn:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }

    .param-empty-state {
      opacity: 0.6;
      padding: 8px 0;
      font-size: 13px;
    }
  `;
}

/**
 * Generate HTML for parameter editor
 */
export function getParameterEditorHTML(
  elementId: string = 'param-editor',
  options: ParameterEditorOptions = {}
): string {
  const {
    showLoadDefaults = true,
    showParameterSource = false,
    namePlaceholder = 'Parameter name',
    valuePlaceholder = 'Value',
  } = options;

  return `
    <div class="param-editor" id="${elementId}">
      <h3>Request Parameters</h3>
      <div class="param-list" id="${elementId}-list"></div>
      <button class="add-param-btn" onclick="addParameter()">+ Add Parameter</button>
      
      ${showLoadDefaults ? `
      <div style="margin-top: 12px;">
        <button class="add-param-btn" onclick="loadDefaults()">Load Defaults</button>
      </div>
      ` : ''}
    </div>
  `;
}

/**
 * Generate JavaScript for parameter editor
 */
export function getParameterEditorJS(options: ParameterEditorOptions = {}): string {
  const {
    showParameterSource = false,
    namePlaceholder = 'Parameter name',
    valuePlaceholder = 'Value',
  } = options;

  return `
    /**
     * Render parameters in the list
     */
    function renderParameters() {
      const listDiv = document.getElementById('param-editor-list');
      if (!listDiv) return;
      
      const params = state.parameters || {};
      
      if (Object.keys(params).length === 0) {
        listDiv.innerHTML = '<div class="param-empty-state">No parameters set. ${options.showLoadDefaults ? 'Click "Load Defaults" or add manually.' : 'Click "+ Add Parameter" to add one.'}</div>';
        return;
      }
      
      listDiv.innerHTML = Object.entries(params).map(([name, value]) => {
        const paramDef = parameterDefinitions.find(p => p.name === name);
        const isRequired = paramDef?.required || false;
        const description = paramDef?.description || '';
        
        return \`
          <div class="param-row" title="\${description}">
            ${showParameterSource ? `
            <select class="param-source" onchange="updateParamSource(this)">
              <option value="query">Query</option>
              <option value="path">Path</option>
              <option value="header">Header</option>
              <option value="body">Body</option>
            </select>
            ` : ''}
            <input 
              type="text" 
              class="param-name" 
              value="\${escapeHtml(name)}" 
              placeholder="${namePlaceholder}"
              list="param-names-list"
              \${isRequired ? 'required' : ''}
            />
            <input 
              type="text" 
              class="param-value" 
              value="\${escapeHtml(value)}" 
              placeholder="${valuePlaceholder}"
              \${isRequired ? 'required' : ''}
            />
            <button onclick="removeParameter(this)" \${isRequired ? 'disabled title="Required parameter"' : ''}>Remove</button>
          </div>
        \`;
      }).join('');
      
      // Populate datalist for autocomplete
      populateParamNamesList();
    }

    /**
     * Populate parameter names datalist for autocomplete
     */
    function populateParamNamesList() {
      let datalist = document.getElementById('param-names-list');
      if (!datalist) {
        datalist = document.createElement('datalist');
        datalist.id = 'param-names-list';
        document.body.appendChild(datalist);
      }
      
      if (parameterDefinitions && parameterDefinitions.length > 0) {
        datalist.innerHTML = parameterDefinitions.map(param => 
          \`<option value="\${escapeHtml(param.name)}">\${escapeHtml(param.description || '')}</option>\`
        ).join('');
      }
    }

    /**
     * Collect parameters from the UI
     */
    function collectParameters() {
      const params = {};
      const rows = document.querySelectorAll('#param-editor-list .param-row');
      
      rows.forEach(row => {
        const name = row.querySelector('.param-name')?.value.trim();
        const value = row.querySelector('.param-value')?.value || '';
        if (name) {
          params[name] = value;
        }
      });
      
      return params;
    }

    /**
     * Add a new parameter row
     */
    function addParameter() {
      state.parameters = collectParameters();
      state.parameters[''] = '';
      renderParameters();
      
      // Focus on the new parameter name input
      setTimeout(() => {
        const inputs = document.querySelectorAll('.param-name');
        if (inputs.length > 0) {
          inputs[inputs.length - 1].focus();
        }
      }, 0);
    }

    /**
     * Remove a parameter row
     */
    function removeParameter(btn) {
      const row = btn.closest('.param-row');
      const name = row.querySelector('.param-name')?.value;
      
      state.parameters = collectParameters();
      delete state.parameters[name];
      renderParameters();
      
      // Notify parent about parameter changes
      vscode.postMessage({
        command: 'updateParams',
        data: { parameters: state.parameters }
      });
    }

    /**
     * Update parameter source (for REST endpoints with showParameterSource)
     */
    function updateParamSource(select) {
      // This can be extended if we need to track parameter sources
      console.log('Parameter source changed:', select.value);
    }

    /**
     * Handle parameter blur event (auto-save)
     */
    function setupParameterAutoSave() {
      const listDiv = document.getElementById('param-editor-list');
      if (!listDiv) return;
      
      listDiv.addEventListener('blur', (e) => {
        if (e.target.classList.contains('param-name') || e.target.classList.contains('param-value')) {
          const params = collectParameters();
          state.parameters = params;
          
          vscode.postMessage({
            command: 'updateParams',
            data: { parameters: params }
          });
        }
      }, true);
    }

    // Initialize parameter auto-save on load
    if (typeof window !== 'undefined') {
      window.addEventListener('load', setupParameterAutoSave);
    }
  `;
}

/**
 * Helper to escape HTML for injection
 */
export function escapeHTML(text: string): string {
  const map: Record<string, string> = {
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#039;'
  };
  return text.replace(/[&<>"']/g, m => map[m]);
}


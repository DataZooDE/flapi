import { bus } from './bus';
import { initializeMonaco, createEditor } from './monaco';
import type { EditorPersistedState, EditorStatePayload } from './lib/types';
import { debounce, tryParseJson } from './lib/util';

const defaultState: EditorPersistedState = {
  tab: 'sql',
  sql: '',
  cacheTemplate: '',
  requestJson: '[]',
  cacheJson: '{}',
  params: '{}',
  paramsResult: '{}',
  autoExpandSql: false,
  autoExpandCache: false,
};

type Section = 'sql' | 'cache' | 'request';

type Editors = {
  sql?: ReturnType<typeof createEditor>;
  cacheTemplate?: ReturnType<typeof createEditor>;
  request?: ReturnType<typeof createEditor>;
  cacheConfig?: ReturnType<typeof createEditor>;
  params?: ReturnType<typeof createEditor>;
  paramsResult?: ReturnType<typeof createEditor>;
};

let state = bus.restoreState(defaultState);
let schema: any;
let monaco: ReturnType<typeof initializeMonaco> | undefined;
const editors: Editors = {};

const queueAutoExpand = debounce((section: 'sql' | 'cache') => {
  if (section === 'sql' && state.autoExpandSql) {
    triggerSqlExpand();
  }
  if (section === 'cache' && state.autoExpandCache) {
    updateCachePreview();
  }
}, 500);

const queueValidation = debounce(() => {
  triggerValidation();
}, 1000);

bootstrap();

function bootstrap() {
  const root = document.getElementById('root') as HTMLElement | null;
  const workerBaseUrl = root?.dataset.workerBase ?? '';

  monaco = initializeMonaco({
    getMustacheVariables: () => collectVariables(),
    getSchema: () => schema,
    workerBaseUrl,
  });
  
  renderLayout();
  setupEditors();
  attachEventHandlers();
  setupResizeHandlers();
  setActiveTab(state.tab);
  syncAutoToggles();
  
  // Set up message handling
  bus.onMessage(handleMessage);
  window.addEventListener('message', (event) => {
    console.log('Webview received window message:', event.data);
    bus.dispatch(event);
  });
  
  console.log('Webview initialized, requesting data...');
  bus.postMessage({ type: 'load' });
  bus.postMessage({ type: 'request-schema' });
  updateCachePreview();
}

function renderLayout() {
  const root = document.getElementById('root');
  if (!root) {
    throw new Error('Missing root element');
  }

  root.innerHTML = `
    <div class="flapi-app">
      <header class="flapi-header">
        <div class="flapi-header__left">
          <h1 id="flapi-endpoint-title">Endpoint</h1>
          <span id="flapi-endpoint-method" class="flapi-badge"></span>
        </div>
        <div class="flapi-header__right">
          <button data-action="refresh">Refresh</button>
        </div>
      </header>
      <nav class="flapi-tabbar">
        <button data-tab="sql" class="active">SQL</button>
        <button data-tab="cache">Cache</button>
        <button data-tab="request">Request</button>
      </nav>
      <section class="flapi-panels">
        <div class="flapi-panel active" data-panel="sql">
          <div class="flapi-section-toolbar">
            <div class="flapi-toolbar-group">
              <button data-action="sql-save">Save</button>
              <button data-action="sql-format">Format</button>
              <button data-action="sql-copy">Copy</button>
            </div>
            <div class="flapi-toolbar-group">
              <label class="flapi-toggle">
                <input type="checkbox" data-action="sql-auto-expand" />
                Auto expand
              </label>
              <button data-action="sql-expand">Preview</button>
              <button data-action="sql-run">Run</button>
            </div>
          </div>
              <div class="flapi-editor-grid">
                <div class="flapi-editor-main-container">
                  <div class="flapi-editor-main" id="sql-editor"></div>
                </div>
                <div class="flapi-resize-handle-vertical" data-resize="vertical"></div>
                <div class="flapi-editor-side-container">
                  <aside class="flapi-editor-side">
                    <section class="flapi-variables">
                      <header>Variables</header>
                      <ul id="sql-variable-list"></ul>
                    </section>
                    <section class="flapi-json-editor">
                      <header>
                        <span>Preview Parameters</span>
                        <button data-action="sql-params-format">Format</button>
                      </header>
                      <div class="flapi-json-container" id="sql-params"></div>
                    </section>
                    <section class="flapi-json-editor">
                      <header>
                        <span>Run Parameters</span>
                        <button data-action="sql-params-result-format">Format</button>
                      </header>
                      <div class="flapi-json-container" id="sql-params-result"></div>
                    </section>
                  </aside>
                </div>
              </div>
              <div class="flapi-resize-handle-horizontal" data-resize="horizontal"></div>
              <div class="flapi-preview-container">
                <div class="flapi-preview">
                  <div class="flapi-preview-tabs">
                    <button data-preview-section="sql" data-preview-target="sql-expanded" class="active">Expanded SQL</button>
                    <button data-preview-section="sql" data-preview-target="sql-result">Result Data</button>
                  </div>
                  <div class="flapi-preview-panels">
                    <pre class="flapi-preview-panel active" data-preview-panel="sql-expanded" id="sql-preview-expanded"></pre>
                    <div class="flapi-preview-panel" data-preview-panel="sql-result" id="sql-preview-result"></div>
                  </div>
                </div>
              </div>
        </div>
        <div class="flapi-panel" data-panel="cache">
          <div class="flapi-section-toolbar">
            <div class="flapi-toolbar-group">
              <button data-action="cache-save">Save</button>
              <button data-action="cache-format">Format</button>
              <button data-action="cache-copy">Copy</button>
            </div>
            <div class="flapi-toolbar-group">
              <label class="flapi-toggle">
                <input type="checkbox" data-action="cache-auto-preview" />
                Auto preview
              </label>
              <button data-action="cache-preview">Preview</button>
            </div>
          </div>
              <div class="flapi-editor-grid">
                <div class="flapi-editor-main-container">
                  <div class="flapi-editor-main" id="cache-editor"></div>
                </div>
                <div class="flapi-resize-handle-vertical" data-resize="vertical"></div>
                <div class="flapi-editor-side-container">
                  <aside class="flapi-editor-side">
                    <section class="flapi-json-editor">
                      <header>
                        <span>Cache Configuration</span>
                        <button data-action="cache-config-format">Format</button>
                      </header>
                      <div class="flapi-json-container" id="cache-config"></div>
                    </section>
                  </aside>
                </div>
              </div>
              <div class="flapi-preview-container">
                <div class="flapi-resize-handle-horizontal" data-resize="horizontal"></div>
                <div class="flapi-preview">
                  <div class="flapi-preview-tabs">
                    <button data-preview-section="cache" data-preview-target="cache-expanded" class="active">Expanded Template</button>
                  </div>
                  <div class="flapi-preview-panels">
                    <pre class="flapi-preview-panel active" data-preview-panel="cache-expanded" id="cache-preview-expanded"></pre>
                  </div>
                </div>
              </div>
        </div>
        <div class="flapi-panel" data-panel="request">
          <div class="flapi-section-toolbar">
            <div class="flapi-toolbar-group">
              <button data-action="request-save">Save</button>
              <button data-action="request-format">Format</button>
            </div>
            <div class="flapi-toolbar-group">
              <span class="flapi-hint">Edit the request schema JSON.</span>
            </div>
          </div>
          <div class="flapi-editor-single" id="request-editor"></div>
        </div>
      </section>
      <footer id="flapi-status"></footer>
    </div>
  `;
}

function setupEditors() {
  editors.sql = createMonacoEditor('sql-editor', 'mustache-sql', state.sql, (value) => {
    state.sql = value;
    bus.persistState(state);
    queueAutoExpand('sql');
    queueValidation();
  });

  editors.cacheTemplate = createMonacoEditor('cache-editor', 'mustache-sql', state.cacheTemplate, (value) => {
    state.cacheTemplate = value;
    bus.persistState(state);
    queueAutoExpand('cache');
  });

  editors.request = createMonacoEditor('request-editor', 'plaintext', state.requestJson, (value) => {
    state.requestJson = value;
    bus.persistState(state);
    updateVariableList();
  });

  editors.cacheConfig = createMonacoEditor('cache-config', 'plaintext', state.cacheJson, (value) => {
    state.cacheJson = value;
    bus.persistState(state);
  }, { height: 200 });

  editors.params = createMonacoEditor('sql-params', 'plaintext', state.params, (value) => {
    state.params = value;
    bus.persistState(state);
    queueAutoExpand('sql');
    queueValidation();
  }, { height: 160 });

  editors.paramsResult = createMonacoEditor('sql-params-result', 'plaintext', state.paramsResult, (value) => {
    state.paramsResult = value;
    bus.persistState(state);
  }, { height: 160 });

  updateVariableList();
}

function attachEventHandlers() {
  document.body.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    if (!target) return;
    const action = target.getAttribute('data-action');
    if (action) {
      handleAction(action);
    }
    const previewTarget = target.getAttribute('data-preview-target');
    const previewSection = target.getAttribute('data-preview-section') as Section | null;
    if (previewSection && previewTarget) {
      setPreviewTab(previewSection, previewTarget);
    }
    const tab = target.getAttribute('data-tab') as Section | null;
    if (tab) {
      setActiveTab(tab);
    }
  });

  document.body.addEventListener('change', (event) => {
    const target = event.target as HTMLInputElement;
    if (!target) return;
    const action = target.getAttribute('data-action');
    if (!action) return;
    if (action === 'sql-auto-expand') {
      state.autoExpandSql = target.checked;
      bus.persistState(state);
      if (target.checked) {
        queueAutoExpand('sql');
      }
    } else if (action === 'cache-auto-preview') {
      state.autoExpandCache = target.checked;
      bus.persistState(state);
      if (target.checked) {
        queueAutoExpand('cache');
      }
    }
  });

  document.addEventListener('keydown', (event) => {
    const isModifier = event.metaKey || event.ctrlKey;
    if (!isModifier) return;
    if (event.key.toLowerCase() === 's') {
      event.preventDefault();
      if (state.tab === 'sql') {
        handleAction('sql-save');
      } else if (state.tab === 'cache') {
        handleAction('cache-save');
      } else {
        handleAction('request-save');
      }
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (state.tab === 'sql') {
        triggerSqlTest();
      }
    }
  });
}

function handleAction(action: string) {
  switch (action) {
    case 'refresh':
      bus.postMessage({ type: 'refresh' });
      break;
    case 'sql-save':
      bus.postMessage({ type: 'save-template', payload: { template: editors.sql?.getValue() ?? '' } });
      break;
    case 'sql-format':
      formatEditor(editors.sql);
      break;
    case 'sql-copy':
      copyToClipboard(editors.sql?.getValue() ?? '');
      break;
    case 'sql-expand':
      triggerSqlExpand();
      break;
    case 'sql-run':
      triggerSqlTest();
      break;
    case 'sql-params-format':
      formatEditor(editors.params);
      break;
    case 'sql-params-result-format':
      formatEditor(editors.paramsResult);
      break;
    case 'cache-save':
      attemptSaveCache();
      break;
    case 'cache-format':
      formatEditor(editors.cacheTemplate);
      break;
    case 'cache-copy':
      copyToClipboard(editors.cacheTemplate?.getValue() ?? '');
      break;
    case 'cache-preview':
      updateCachePreview();
      break;
    case 'cache-config-format':
      formatEditor(editors.cacheConfig);
      break;
    case 'request-save':
      attemptSaveRequest();
      break;
    case 'request-format':
      formatEditor(editors.request);
      break;
    default:
      break;
  }
}

function handleMessage(message: { type: string; payload?: any }) {
  console.log('Webview received message:', message.type, message.payload);
  switch (message.type) {
    case 'load-result':
      console.log('Applying load result:', message.payload);
      applyLoad(message.payload as EditorStatePayload);
      break;
        case 'schema-load':
          schema = message.payload;
          schemaData = message.payload;
          break;
    case 'save-template-success':
      setStatus('Template saved');
      break;
    case 'save-cache-success':
      setStatus('Cache saved');
      break;
    case 'save-request-success':
      setStatus('Request saved');
      break;
        case 'expand-template-success':
          updateSqlPreview(message.payload);
          if (message.payload?.variables) {
            updateVariableListFromApi(message.payload.variables);
          }
          setPreviewTab('sql', 'sql-expanded');
          break;
        case 'test-template-success':
          updateSqlResult(message.payload);
          setPreviewTab('sql', 'sql-result');
          break;
        case 'validate-template-success':
          handleValidationResult(message.payload);
          break;
        case 'error':
          setStatus(message.payload?.message ?? 'Unknown error', 'error');
          break;
    default:
      break;
  }
}

function applyLoad(payload: EditorStatePayload) {
  state.sql = payload.template ?? state.sql;
  state.cacheTemplate = payload.cacheTemplate ?? state.cacheTemplate;
  state.requestJson = JSON.stringify(payload.config?.request ?? [], null, 2);
  state.cacheJson = JSON.stringify(payload.cache ?? {}, null, 2);
  bus.persistState(state);

  editors.sql?.setValue(state.sql);
  editors.cacheTemplate?.setValue(state.cacheTemplate);
  editors.request?.setValue(state.requestJson);
  editors.cacheConfig?.setValue(state.cacheJson);
  editors.params?.setValue(state.params);
  editors.paramsResult?.setValue(state.paramsResult);

  updateVariableList();
  updateCachePreview();

  const titleEl = document.getElementById('flapi-endpoint-title');
  const methodEl = document.getElementById('flapi-endpoint-method');
  if (titleEl) titleEl.textContent = payload.path;
  if (methodEl) methodEl.textContent = payload.config?.method ?? 'GET';
}

function attemptSaveRequest() {
  const parsed = tryParseJson(state.requestJson, []);
  if (parsed.error) {
    setStatus(`Invalid request JSON: ${parsed.error}`, 'error');
    return;
  }
  bus.postMessage({ type: 'save-request', payload: { config: { request: parsed.data } } });
}

function attemptSaveCache() {
  const parsed = tryParseJson(state.cacheJson, {});
  if (parsed.error) {
    setStatus(`Invalid cache JSON: ${parsed.error}`, 'error');
    return;
  }
  bus.postMessage({
    type: 'save-cache',
    payload: {
      cache: parsed.data,
      cacheTemplate: state.cacheTemplate,
    },
  });
}

function triggerSqlExpand() {
  const parsed = tryParseJson(state.params, {});
  if (parsed.error) {
    setStatus(`Invalid parameters JSON: ${parsed.error}`, 'error');
    return;
  }
  bus.postMessage({ type: 'expand-template', payload: { parameters: parsed.data, includeVariables: true } as any });
}

function triggerSqlTest() {
  const parsed = tryParseJson(state.paramsResult, {});
  if (parsed.error) {
    setStatus(`Invalid parameters JSON: ${parsed.error}`, 'error');
    return;
  }
  bus.postMessage({ type: 'test-template', payload: { parameters: parsed.data } });
}

function updateSqlPreview(payload: any) {
  const expandedEl = document.getElementById('sql-preview-expanded');
  if (!expandedEl) return;
  const text = typeof payload?.expanded === 'string' ? payload.expanded : JSON.stringify(payload, null, 2);
  expandedEl.textContent = text;
}

function updateSqlResult(payload: any) {
  const resultEl = document.getElementById('sql-preview-result');
  if (!resultEl) return;
  if (!payload) {
    resultEl.innerHTML = '<em>No result returned.</em>';
    return;
  }

  let columns: string[] = [];
  let rows: any[] = [];

  if (Array.isArray(payload?.columns) && Array.isArray(payload?.rows)) {
    columns = payload.columns.map((col: any) => col.name ?? String(col));
    rows = payload.rows;
  } else if (Array.isArray(payload)) {
    rows = payload;
  } else if (Array.isArray(payload?.data)) {
    rows = payload.data;
  }

  if (!columns.length && rows.length) {
    if (Array.isArray(rows[0])) {
      columns = rows[0].map((_: unknown, index: number) => `col${index + 1}`);
    } else if (typeof rows[0] === 'object' && rows[0] !== null) {
      columns = Object.keys(rows[0]);
    }
  }

  if (!columns.length) {
    resultEl.textContent = JSON.stringify(payload, null, 2);
    return;
  }

  const header = columns.map((col) => `<th>${escapeHtml(col)}</th>`).join('');
  const body = rows
    .map((row) => {
      let cells: string[];
      if (Array.isArray(row)) {
        cells = row.map((value) => `<td>${escapeHtml(String(value ?? ''))}</td>`);
      } else {
        cells = columns.map((col) => `<td>${escapeHtml(String(row?.[col] ?? ''))}</td>`);
      }
      return `<tr>${cells.join('')}</tr>`;
    })
    .join('');

  const info = `<div class="flapi-result-meta">Rows: ${rows.length}</div>`;
  resultEl.innerHTML = `${info}<div class="flapi-table-wrap"><table><thead><tr>${header}</tr></thead><tbody>${body}</tbody></table></div>`;
}

function updateCachePreview() {
  const cachePreview = document.getElementById('cache-preview-expanded');
  if (!cachePreview) return;
  if (!state.cacheTemplate?.trim()) {
    cachePreview.innerHTML = '<em>No cache template defined.</em>';
    return;
  }
  cachePreview.textContent = state.cacheTemplate;
}

function updateVariableList() {
  const list = document.getElementById('sql-variable-list');
  if (!list) return;
  let items: string[] = [];
  try {
    const parsed = JSON.parse(state.requestJson || '[]');
    if (Array.isArray(parsed)) {
      items = parsed.map((field) => field['field-name'] ?? field.fieldName).filter(Boolean);
    }
  } catch {
    // ignore parse errors; list will show message
  }
  if (!items.length) {
    list.innerHTML = '<li class="flapi-hint">No request parameters defined.</li>';
    return;
  }
  list.innerHTML = items
    .map((name) => `<li><code>{{params.${escapeHtml(name)}}}</code></li>`)
    .concat(['<li><code>{{conn.*}}</code></li>', '<li><code>{{env.*}}</code></li>', '<li><code>{{cache.*}}</code></li>'])
    .join('');
}

function updateVariableListFromApi(variables: any) {
  // Store discovered variables for completion
  discoveredVariables = variables;
  
  const list = document.getElementById('sql-variable-list');
  if (!list) return;
  
  const items: string[] = [];
  
  // Add request parameters
  if (variables.params) {
    Object.keys(variables.params).forEach(name => {
      const variable = variables.params[name];
      items.push(`<li><code>{{params.${escapeHtml(name)}}}</code> <span class="flapi-var-value">${escapeHtml(variable.value)}</span></li>`);
    });
  }
  
  // Add connection variables
  if (variables.conn) {
    Object.keys(variables.conn).forEach(name => {
      const variable = variables.conn[name];
      items.push(`<li><code>{{conn.${escapeHtml(name)}}}</code> <span class="flapi-var-value">${escapeHtml(variable.value)}</span></li>`);
    });
  }
  
  // Add environment variables
  if (variables.env) {
    Object.keys(variables.env).forEach(name => {
      const variable = variables.env[name];
      items.push(`<li><code>{{env.${escapeHtml(name)}}}</code> <span class="flapi-var-value">${escapeHtml(variable.value)}</span></li>`);
    });
  }
  
  // Add cache variables
  if (variables.cache) {
    Object.keys(variables.cache).forEach(name => {
      const variable = variables.cache[name];
      items.push(`<li><code>{{cache.${escapeHtml(name)}}}</code> <span class="flapi-var-value">${escapeHtml(variable.value)}</span></li>`);
    });
  }
  
  if (items.length === 0) {
    list.innerHTML = '<li class="flapi-hint">No variables available. Click "Preview" to discover variables.</li>';
    return;
  }
  
  list.innerHTML = items.join('');
}

function setActiveTab(tab: Section) {
  state.tab = tab;
  bus.persistState(state);
  document.querySelectorAll('[data-tab]').forEach((button) => {
    button.classList.toggle('active', button.getAttribute('data-tab') === tab);
  });
  document.querySelectorAll('[data-panel]').forEach((panel) => {
    panel.classList.toggle('active', panel.getAttribute('data-panel') === tab);
  });
}

function setPreviewTab(section: Section, target: string) {
  document.querySelectorAll(`[data-preview-section="${section}"]`).forEach((button) => {
    button.classList.toggle('active', button.getAttribute('data-preview-target') === target);
  });
  document.querySelectorAll(`[data-preview-panel]`).forEach((panel) => {
    const match = panel.getAttribute('data-preview-panel') === target;
    if (match) {
      panel.classList.add('active');
    } else if (panel.getAttribute('data-preview-panel')?.startsWith(`${section}-`)) {
      panel.classList.remove('active');
    }
  });
}

function syncAutoToggles() {
  const sqlToggle = document.querySelector<HTMLInputElement>('input[data-action="sql-auto-expand"]');
  if (sqlToggle) {
    sqlToggle.checked = state.autoExpandSql;
  }
  const cacheToggle = document.querySelector<HTMLInputElement>('input[data-action="cache-auto-preview"]');
  if (cacheToggle) {
    cacheToggle.checked = state.autoExpandCache;
  }
}

function createMonacoEditor(id: string, language: string, value: string, onChange: (value: string) => void, options?: { height?: number }) {
  const container = document.getElementById(id) as HTMLElement | null;
  if (!container) {
    throw new Error(`Missing editor container: ${id}`);
  }
  if (options?.height) {
    container.style.height = `${options.height}px`;
  }
  
  try {
    const instance = createEditor(container, {
      value,
      language,
      automaticLayout: true,
      minimap: { enabled: false },
      wordWrap: 'on',
      scrollBeyondLastLine: false,
    });
    instance.editor.onDidChangeModelContent(() => onChange(instance.getValue()));
    return instance;
  } catch (error) {
    console.error(`Failed to create Monaco editor for ${id}:`, error);
    // Fallback: create a simple textarea
    const textarea = document.createElement('textarea');
    textarea.value = value;
    textarea.style.width = '100%';
    textarea.style.height = '100%';
    textarea.style.border = 'none';
    textarea.style.resize = 'none';
    textarea.style.fontFamily = 'monospace';
    textarea.style.fontSize = '13px';
    textarea.style.padding = '8px';
    textarea.addEventListener('input', () => onChange(textarea.value));
    container.appendChild(textarea);
    
    return {
      editor: null as any,
      dispose: () => textarea.remove(),
      getValue: () => textarea.value,
      setValue: (val: string) => { textarea.value = val; },
      layout: () => {},
    };
  }
}

function formatEditor(instance?: ReturnType<typeof createEditor>) {
  instance?.editor.getAction('editor.action.formatDocument')?.run()?.catch(() => {
    // ignore formatter errors
  });
}

function copyToClipboard(value: string) {
  if (!navigator.clipboard) {
    setStatus('Clipboard API unavailable', 'error');
    return;
  }
  navigator.clipboard.writeText(value).then(() => {
    setStatus('Copied to clipboard');
  }).catch((error) => {
    setStatus(`Copy failed: ${String(error)}`, 'error');
  });
}

let discoveredVariables: any = null;
let schemaData: any = null;
let validationErrors: any[] = [];
let validationWarnings: any[] = [];

function collectVariables(): string[] {
  // If we have discovered variables from the API, use those
  if (discoveredVariables) {
    const variables: string[] = [];
    
    if (discoveredVariables.params) {
      Object.keys(discoveredVariables.params).forEach(name => {
        variables.push(`params.${name}`);
      });
    }
    
    if (discoveredVariables.conn) {
      Object.keys(discoveredVariables.conn).forEach(name => {
        variables.push(`conn.${name}`);
      });
    }
    
    if (discoveredVariables.env) {
      Object.keys(discoveredVariables.env).forEach(name => {
        variables.push(`env.${name}`);
      });
    }
    
    if (discoveredVariables.cache) {
      Object.keys(discoveredVariables.cache).forEach(name => {
        variables.push(`cache.${name}`);
      });
    }
    
    return variables;
  }
  
  // Fallback to request field parsing
  try {
    const parsed = JSON.parse(state.requestJson || '[]');
    if (!Array.isArray(parsed)) {
      return ['conn.*', 'env.*', 'cache.*'];
    }
    return parsed
      .map((field) => field['field-name'] ?? field.fieldName)
      .filter(Boolean)
      .map((name: string) => `params.${name}`)
      .concat(['conn.*', 'env.*', 'cache.*']);
  } catch {
    return ['conn.*', 'env.*', 'cache.*'];
  }
}

function setStatus(message: string, kind: 'info' | 'error' = 'info') {
  const statusEl = document.getElementById('flapi-status');
  if (!statusEl) return;
  statusEl.textContent = message;
  statusEl.dataset.kind = kind;
  if (message) {
    setTimeout(() => {
      if (statusEl.textContent === message) {
        statusEl.textContent = '';
        statusEl.dataset.kind = '';
      }
    }, 4000);
  }
}

  function triggerValidation() {
    const parsed = tryParseJson(state.params, {});
    if (parsed.error) {
      return; // Don't validate if parameters are invalid JSON
    }
    bus.postMessage({ type: 'validate-template' as any, payload: { parameters: parsed.data } });
  }

  function handleValidationResult(payload: any) {
    validationErrors = payload.errors || [];
    validationWarnings = payload.warnings || [];
    
    // Update validation display
    updateValidationDisplay();
    
    // Update Monaco editor markers if available
    if (editors.sql?.editor) {
      updateEditorMarkers(editors.sql.editor, validationErrors, validationWarnings);
    }
  }

  function updateValidationDisplay() {
    const statusEl = document.getElementById('flapi-status');
    if (!statusEl) return;
    
    if (validationErrors.length > 0) {
      const errorCount = validationErrors.length;
      const warningCount = validationWarnings.length;
      let message = `${errorCount} error${errorCount !== 1 ? 's' : ''}`;
      if (warningCount > 0) {
        message += `, ${warningCount} warning${warningCount !== 1 ? 's' : ''}`;
      }
      setStatus(message, 'error');
    } else if (validationWarnings.length > 0) {
      const warningCount = validationWarnings.length;
      setStatus(`${warningCount} warning${warningCount !== 1 ? 's' : ''}`, 'info');
    } else {
      // Clear status if no errors/warnings
      setStatus('');
    }
  }

  function updateEditorMarkers(editor: any, errors: any[], warnings: any[]) {
    if (!monaco) return;
    
    const model = editor.getModel();
    if (!model) return;
    
    const markers: any[] = [];
    
    // Add error markers
    errors.forEach((error: any) => {
      markers.push({
        startLineNumber: 1,
        startColumn: 1,
        endLineNumber: 1,
        endColumn: 1,
        message: error.message || 'Validation error',
        severity: monaco!.MarkerSeverity.Error,
      });
    });
    
    // Add warning markers
    warnings.forEach((warning: any) => {
      markers.push({
        startLineNumber: 1,
        startColumn: 1,
        endLineNumber: 1,
        endColumn: 1,
        message: warning.message || 'Validation warning',
        severity: monaco!.MarkerSeverity.Warning,
      });
    });
    
    monaco!.editor.setModelMarkers(model, 'flapi-validation', markers);
  }

  function setupResizeHandlers() {
    const verticalHandles = document.querySelectorAll('.flapi-resize-handle-vertical');
    const horizontalHandles = document.querySelectorAll('.flapi-resize-handle-horizontal');
    
    verticalHandles.forEach(handle => {
      setupVerticalResize(handle as HTMLElement);
    });
    
    horizontalHandles.forEach(handle => {
      setupHorizontalResize(handle as HTMLElement);
    });
  }

  function setupVerticalResize(handle: HTMLElement) {
    let isResizing = false;
    let startX = 0;
    let startWidth = 0;
    let sideContainer: HTMLElement | null = null;

    handle.addEventListener('mousedown', (e) => {
      isResizing = true;
      startX = e.clientX;
      sideContainer = handle.nextElementSibling as HTMLElement;
      if (sideContainer) {
        startWidth = sideContainer.offsetWidth;
      }
      handle.classList.add('dragging');
      document.body.style.cursor = 'col-resize';
      document.body.style.userSelect = 'none';
      e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
      if (!isResizing || !sideContainer) return;
      
      const deltaX = e.clientX - startX; // Fixed: drag right = expand, drag left = shrink
      const newWidth = Math.max(150, Math.min(window.innerWidth * 0.5, startWidth - deltaX));
      sideContainer.style.width = `${newWidth}px`;
      
      // Trigger Monaco layout update
      setTimeout(() => {
        Object.values(editors).forEach(editor => {
          if (editor?.editor) {
            editor.layout();
          }
        });
      }, 0);
    });

    document.addEventListener('mouseup', () => {
      if (isResizing) {
        isResizing = false;
        handle.classList.remove('dragging');
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
      }
    });
  }

  function setupHorizontalResize(handle: HTMLElement) {
    let isResizing = false;
    let startY = 0;
    let startHeight = 0;
    let previewContainer: HTMLElement | null = null;

    handle.addEventListener('mousedown', (e) => {
      isResizing = true;
      startY = e.clientY;
      previewContainer = handle.nextElementSibling as HTMLElement;
      if (previewContainer) {
        startHeight = previewContainer.offsetHeight;
      }
      handle.classList.add('dragging');
      document.body.style.cursor = 'row-resize';
      document.body.style.userSelect = 'none';
      e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
      if (!isResizing || !previewContainer) return;
      
      const deltaY = e.clientY - startY;
      const newHeight = Math.max(100, Math.min(window.innerHeight * 0.6, startHeight + deltaY));
      previewContainer.style.height = `${newHeight}px`;
      
      // Debug logging
      console.log('Horizontal resize:', { deltaY, startHeight, newHeight, currentY: e.clientY, startY });
    });

    document.addEventListener('mouseup', () => {
      if (isResizing) {
        isResizing = false;
        handle.classList.remove('dragging');
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
      }
    });
  }

  function escapeHtml(value: string): string {
    return value
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

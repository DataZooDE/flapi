# Endpoint Tester Refactoring Plan

## Goal
Align the Endpoint Tester UI with the SQL Tester's better design, making sections resizable and collapsible.

## Current State vs Target State

### Current Layout (Endpoint Tester)
```
[Header: Request]
  URL Bar + Send button
  Query Parameters (always visible)
  <details> Authentication
  <details> Headers
  <details> Request Body

[Header: Response]
  Status Bar
  Tabs: Body | Headers
  Tab Content
  Action Buttons
```

### Target Layout (Following SQL Tester Pattern)
```
┌─────────────────────────────────┐
│ Header (Fixed)                  │
│  - Title: "REST Endpoint Tester"│
│  - Endpoint info                │
├─────────────────────────────────┤
│ URL Bar Section                 │
│  GET [URL field] [Send 🚀]      │
├─────────────────────────────────┤
│ ▶ Query Parameters (Collapsible)│
│   [param list]                  │
├─────────────────────────────────┤
│ ▶ Authentication (Collapsible)  │
│   [auth fields]                 │
├─────────────────────────────────┤
│ ▶ Headers (Collapsible)         │
│   [header list]                 │
├─────────────────────────────────┤
│ ▶ Request Body (Collapsible)    │
│   [JSON editor]                 │
├═════════════════════════════════┤  ← Resize Handle
│ Response Section                │
│  [Status: Time, Size, Rows]    │
│  [Response content]            │
│  [JSON | Table toggle]         │
└─────────────────────────────────┘
```

## Key Changes Needed

### 1. Header Section
- Change from two separate sections to one unified layout
- Add endpoint metadata (URL path, method)
- Use consistent header styling

### 2. URL Bar
- Move to top of request section (prominent position)
- Style consistently with icon buttons

### 3. Collapsible Sections
Replace `<details>` with custom collapsible implementation:
- Consistent expand icon (▶/▼)
- Hover effect on header
- Smooth transitions
- Remember collapsed state

### 4. Resize Handles
Add between major sections:
- Between Request area and Response area
- 4px transparent handle with hover effect
- Drag functionality with minimum heights

### 5. Action Buttons
Replace emoji buttons with Codicons:
- 📋 Copy → `codicon-copy`
- ✨ Format → `codicon-symbol-field`
- 📚 History → `codicon-history`
- 🚀 Send → `codicon-play`

### 6. Response Section
- Keep tabs (Body/Headers) but style consistently
- Ensure JSON/Table toggle matches SQL tester
- Response info bar at top (not middle)

## Implementation Steps

### Step 1: Update HTML Structure
```html
<div class="container">
  <div class="header">
    <h2>REST Endpoint Tester</h2>
    <div class="header-info">
      <div>Endpoint: <code id="endpoint-path"></code></div>
      <div>Method: <code id="endpoint-method"></code></div>
    </div>
  </div>

  <div class="main-content">
    <!-- URL Bar (always visible) -->
    <div class="url-section">
      <select id="method">...</select>
      <input id="url" />
      <button id="sendBtn">
        <span class="codicon codicon-play"></span>
      </button>
    </div>

    <!-- Collapsible Sections -->
    <div class="collapsible-section">
      <div class="collapsible-header" onclick="toggleSection('params')">
        <div>
          <span class="expand-icon" id="params-icon">▶</span>
          <strong>Query Parameters</strong>
        </div>
      </div>
      <div class="collapsible-content" id="params-content">
        <!-- params content -->
      </div>
    </div>

    <!-- Similar for Auth, Headers, Body -->
    
  </div>

  <!-- Resize Handle -->
  <div class="resize-handle" data-resize="vertical"></div>

  <!-- Response Section -->
  <div class="response-section">
    <div class="section-header">Response</div>
    <div class="response-info" id="response-info">...</div>
    <div class="section-content">
      <!-- response tabs and content -->
    </div>
  </div>
</div>
```

### Step 2: Update CSS
Add from SQL tester:
- `.header` and `.header-info` styles
- `.collapsible-section`, `.collapsible-header`, `.collapsible-content`
- `.resize-handle` styles
- `.section-header` consistent styling
- `.response-info` bar styling
- Icon button styles (`.action-button`)

### Step 3: Add JavaScript Functionality
```javascript
// Collapsible sections
function toggleSection(sectionId) {
  const content = document.getElementById(`${sectionId}-content`);
  const icon = document.getElementById(`${sectionId}-icon`);
  const isExpanded = content.classList.contains('expanded');
  
  if (isExpanded) {
    content.classList.remove('expanded');
    icon.textContent = '▶';
  } else {
    content.classList.add('expanded');
    icon.textContent = '▼';
  }
  
  // Save state
  localStorage.setItem(`${sectionId}-expanded`, !isExpanded);
}

// Resize functionality
function initializeResize() {
  const handles = document.querySelectorAll('.resize-handle');
  handles.forEach(handle => {
    let startY, startHeight;
    
    handle.addEventListener('mousedown', (e) => {
      handle.classList.add('dragging');
      startY = e.clientY;
      const section = handle.previousElementSibling;
      startHeight = section.offsetHeight;
      
      document.addEventListener('mousemove', onMouseMove);
      document.addEventListener('mouseup', onMouseUp);
    });
    
    function onMouseMove(e) {
      const delta = e.clientY - startY;
      const section = handle.previousElementSibling;
      section.style.height = `${Math.max(100, startHeight + delta)}px`;
    }
    
    function onMouseUp() {
      handle.classList.remove('dragging');
      document.removeEventListener('mousemove', onMouseMove);
      document.removeEventListener('mouseup', onMouseUp);
    }
  });
}
```

### Step 4: Migrate Icons
Replace:
- `Send ▶️` → `<span class="codicon codicon-play"></span>` + title
- `📋 Copy` → `<span class="codicon codicon-copy"></span>` + title
- `✨ Format` → `<span class="codicon codicon-symbol-field"></span>` + title
- `📚 History` → `<span class="codicon codicon-history"></span>` + title

## File Changes Required

1. `/src/webview/endpointTesterPanel.ts`
   - Update `_getHtmlForWebview()` method
   - Update `_getStyles()` method  
   - Update `_getScript()` method

## Testing Checklist
- [ ] All sections collapse/expand correctly
- [ ] Expand state persists on refresh
- [ ] Resize handle works smoothly
- [ ] Minimum heights enforced
- [ ] Icons display correctly
- [ ] Send request still works
- [ ] Response display unaffected
- [ ] JSON/Table toggle works
- [ ] Pagination still works
- [ ] History panel still works

## Notes
- Maintain backward compatibility with existing state management
- Don't break existing functionality (parameters, auth, pagination, etc.)
- Test with both REST and MCP endpoints
- Ensure responsive behavior


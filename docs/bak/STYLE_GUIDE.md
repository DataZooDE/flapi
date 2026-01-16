# Flapi VSCode Extension - UI Style Guide

## Design Principles

1. **Consistency**: All webview panels should follow the same visual language
2. **VSCode Integration**: Use VSCode theme variables for colors and styling
3. **Flexibility**: Sections should be resizable and collapsible for different workflows
4. **Clarity**: Clear visual hierarchy with consistent spacing and typography

## Layout Structure

### Standard Panel Layout

All tester/editor panels should follow this structure:

```
┌─────────────────────────────────────┐
│ Header (Fixed)                      │
│ - Title                             │
│ - Metadata (paths, config info)    │
├─────────────────────────────────────┤
│ Collapsible Section 1               │
│ ▶ Section Title                     │
│   (expandable content)              │
├─────────────────────────────────────┤
│ Main Content Area (Scrollable)      │
│ - Parameters/Request Fields         │
│ - Actions (buttons with icons)     │
├─────────────────────────────────────┤
│ Preview/Editor Section (Resizable)  │
│ - Syntax highlighted                │
│ - Collapsible                       │
├─────────────────────────────────────┤
│ Results Section (Resizable)         │
│ - Response metadata bar             │
│ - Toggle: JSON / Table view         │
│ - Data display                      │
└─────────────────────────────────────┘
```

## Color Palette

Use VSCode CSS variables for consistency:

### Backgrounds
- `--vscode-editor-background` - Main content areas
- `--vscode-sideBar-background` - Headers, metadata bars
- `--vscode-textCodeBlock-background` - Code/preview sections

### Borders
- `--vscode-panel-border` - Section dividers (1px solid)

### Text
- `--vscode-foreground` - Primary text
- `--vscode-descriptionForeground` - Secondary text/hints

### Interactive Elements
- `--vscode-button-background` / `--vscode-button-foreground` - Primary buttons
- `--vscode-button-secondaryBackground` / `--vscode-button-secondaryForeground` - Secondary buttons
- `--vscode-list-hoverBackground` - Hover states

## Typography

### Font Families
- UI Text: `var(--vscode-font-family)`
- Code/Monospace: `var(--vscode-editor-font-family)`

### Font Sizes
- Headers (h2): `16px`, `font-weight: 600`
- Section Headers (h3): `13px`, `font-weight: 600`
- Body Text: `var(--vscode-font-size)` (typically 13px)
- Small Text/Hints: `12px`, `opacity: 0.8`
- Code: `12px` monospace

## Spacing

### Padding/Margins
- Section padding: `12px 16px`
- Compact sections: `8px 16px`
- Content margins: `12px` between elements
- Button gaps: `8px`

### Layout
- Min section height: `80px`
- Resize handle: `4px` with `±2px` hover area

## Component Patterns

### Section Headers
```css
.section-header {
  padding: 8px 16px;
  background-color: var(--vscode-sideBar-background);
  border-bottom: 1px solid var(--vscode-panel-border);
  font-weight: 600;
  font-size: 13px;
}
```

### Collapsible Sections
```css
.collapsible-header {
  padding: 8px 16px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  cursor: pointer;
  user-select: none;
}

.collapsible-header:hover {
  background-color: var(--vscode-list-hoverBackground);
}

.expand-icon {
  transition: transform 0.2s;
}

.expand-icon.expanded {
  transform: rotate(90deg);
}
```

### Resize Handles
```css
.resize-handle {
  height: 4px;
  background: transparent;
  cursor: row-resize;
  position: relative;
  flex-shrink: 0;
}

.resize-handle:hover,
.resize-handle.dragging {
  background: var(--vscode-focusBorder);
}

.resize-handle::before {
  content: '';
  position: absolute;
  top: -2px;
  bottom: -2px;
  left: 0;
  right: 0;
}
```

### Buttons with Icons
```css
.action-button {
  padding: 8px 12px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  min-width: 40px;
  height: 32px;
  background: var(--vscode-button-background);
  color: var(--vscode-button-foreground);
}

.action-button .codicon {
  font-size: 16px;
}

.action-button:hover {
  background: var(--vscode-button-hoverBackground);
}
```

### View Toggle (JSON/Table)
```css
.view-toggle-bottom {
  display: flex;
  gap: 4px;
  padding: 8px;
  border-top: 1px solid var(--vscode-panel-border);
  background: var(--vscode-sideBar-background);
  justify-content: center;
}

.view-toggle-btn {
  padding: 6px 16px;
  border: 1px solid var(--vscode-button-border);
  background: var(--vscode-button-secondaryBackground);
  color: var(--vscode-button-secondaryForeground);
  border-radius: 3px;
  cursor: pointer;
  font-size: 12px;
}

.view-toggle-btn.active {
  background: var(--vscode-button-background);
  color: var(--vscode-button-foreground);
  border-color: var(--vscode-button-background);
}
```

### Response Metadata Bar
```css
.response-info {
  display: flex;
  gap: 16px;
  padding: 8px 12px;
  background: var(--vscode-sideBar-background);
  border-bottom: 1px solid var(--vscode-panel-border);
  font-size: 12px;
}

.response-info span {
  opacity: 0.8;
}

.response-info strong {
  margin-left: 4px;
  opacity: 1;
}
```

## Syntax Highlighting

### SQL
Use consistent color classes:
- `.sql-keyword` - Blue, bold (SELECT, FROM, WHERE)
- `.sql-function` - Yellow (COUNT, SUM)
- `.sql-string` - Orange (string literals)
- `.sql-number` - Light green (numeric literals)
- `.sql-comment` - Gray, italic (comments)

### JSON
Use VSCode's built-in JSON formatting with proper indentation (2 spaces)

## Tables

### Data Tables
```css
table {
  border-collapse: collapse;
  width: 100%;
  font-size: 12px;
}

th, td {
  padding: 6px 8px;
  text-align: left;
  border: 1px solid var(--vscode-panel-border);
  max-width: 300px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

th {
  background: var(--vscode-sideBar-background);
  font-weight: 600;
  position: sticky;
  top: 0;
  z-index: 1;
}

tr:hover {
  background: var(--vscode-list-hoverBackground);
}
```

## Icons

Use VSCode Codicons for consistency:
- Preview: `codicon-eye`
- Execute/Run: `codicon-play`
- Refresh: `codicon-refresh`
- Copy: `codicon-copy`
- Format: `codicon-symbol-field`
- Expand: `codicon-chevron-right` / `codicon-chevron-down`

## Accessibility

- All interactive elements must be keyboard accessible
- Use semantic HTML (`<button>`, `<details>`, `<summary>`)
- Provide title attributes for icon-only buttons
- Maintain proper focus states
- Ensure sufficient color contrast (VSCode variables handle this)

## Examples

See these reference implementations:
- `/src/webview/sqlTemplateTesterPanel.ts` - **Primary reference**
- `/webview/src/index.ts` - Endpoint editor (being updated to match)

## Migration Checklist

When updating existing panels:

- [ ] Use consistent section structure (header, content, actions)
- [ ] Add collapsible sections with expand icons
- [ ] Add resize handles between major sections
- [ ] Use Codicons for all action buttons
- [ ] Implement JSON/Table toggle for results
- [ ] Apply consistent spacing (12px/16px)
- [ ] Use VSCode CSS variables throughout
- [ ] Add syntax highlighting where appropriate
- [ ] Test resize functionality
- [ ] Test collapse/expand functionality


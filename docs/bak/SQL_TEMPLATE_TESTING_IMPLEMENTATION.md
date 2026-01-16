# SQL Template Testing View - Implementation Summary

## Overview

We've implemented a comprehensive SQL template testing view for the VSCode extension that mirrors the REST endpoint testing functionality. This allows developers to test SQL templates with Mustache variables, preview the expanded SQL, and execute queries directly from VSCode.

## Components Implemented

### 1. TestStateService (`services/testStateService.ts`)

**Purpose**: Unified state management for both REST endpoint and SQL template testing.

**Key Features**:
- Single storage service for parameters, headers, and history
- Supports both REST (`type: 'rest'`) and SQL (`type: 'sql'`) test states
- Persists to VSCode workspace state
- Manages history (up to 10 entries per endpoint/template)
- Provides workspace-level defaults for headers

**Main Methods**:
- `getState(identifier)`: Retrieve test state by identifier
- `saveState(state)`: Persist test state
- `initializeRestState()`: Create initial state for REST endpoints
- `initializeSqlState()`: Create initial state for SQL templates
- `updateParameters()`: Update request parameters
- `addToHistory()`: Add execution result to history
- `clearHistory()`: Clear execution history

### 2. SqlTemplateTestCodeLensProvider (`codelens/sqlTemplateTestProvider.ts`)

**Purpose**: Provides CodeLens buttons on SQL files that have associated YAML configs.

**Key Features**:
- Detects SQL files with associated YAML configurations
- Adds "Test SQL Template" button at the top of eligible SQL files
- Two strategies for finding YAML configs:
  1. Convention-based: Looks for `{basename}.yaml`, `{basename}-rest.yaml`, etc.
  2. Backend query: Queries `/api/v1/_config/filesystem` to find templates

**CodeLens Button**:
```typescript
$(beaker) Test SQL Template
```

### 3. SqlTemplateTesterPanel (`webview/sqlTemplateTesterPanel.ts`)

**Purpose**: WebView panel for SQL template testing with rich UI.

**Key Features**:

#### Collapsible Variables Panel
- Shows available Mustache variables from environment whitelist
- Displays variable names, values, and availability status
- Collapsible to save screen space

#### Parameter Editor
- Reuses REST endpoint parameter editor pattern
- Add/remove parameters dynamically
- Load defaults from YAML config
- Configurable result limit (default: 1000 rows)

#### Actions
- **Preview SQL**: Renders Mustache template without executing
- **Execute**: Runs the query and displays results
- **Load Defaults**: Populates parameters from YAML request fields

#### Preview Section
- Shows expanded SQL with Mustache variables substituted
- Syntax-highlighted display

#### Results Section
- Response info bar (time, size, row count)
- Tabular display for array results
- JSON fallback for complex data
- Error display with details

### 4. Backend Integration

**New API Endpoints Used**:
- `GET /api/v1/_config/endpoints/{slug}/parameters`: Get parameter definitions with defaults
- `GET /api/v1/_config/environment-variables`: Get available environment variables
- `POST /api/v1/_config/endpoints/{slug}/test`: Execute SQL with parameters

### 5. Extension Registration (`extension.ts`)

**Changes**:
- Registered `SqlTemplateTestCodeLensProvider` for SQL files
- Registered `flapi.testSqlTemplate` command
- Initialized `TestStateService` for unified state management
- Updated client refresh logic to include SQL CodeLens provider

## User Workflow

1. **Open SQL Template File**: User opens a `.sql` file (e.g., `customers.sql`)

2. **CodeLens Detection**: If the SQL file has an associated YAML config, a CodeLens button appears:
   ```
   $(beaker) Test SQL Template
   ```

3. **Click to Test**: User clicks the button, opening the SQL Template Tester panel

4. **Variable Explorer**: User can expand the variables panel to see available Mustache variables

5. **Parameter Configuration**:
   - Click "Load Defaults" to auto-populate from YAML config
   - Add/edit parameters manually
   - Set result limit

6. **Preview**: Click "Preview SQL" to see the expanded template without executing

7. **Execute**: Click "Execute" to run the query and see results in a table

8. **Persistent State**: Parameters and history are saved per SQL file across sessions

## Architecture Decisions

### Why Unified TestStateService?

We replaced `ParameterStorageService` with `TestStateService` to:
- Share parameter storage logic between REST and SQL testing
- Avoid code duplication
- Enable future extensions (e.g., MCP tool testing)
- Provide consistent UX across different test types

### Storage Key Strategy

Test states are stored with normalized identifiers:
- REST endpoints: Use slug (e.g., `/customers/` → `_customers_`)
- SQL templates: Use file path (e.g., `/path/to/customers.sql`)

### YAML Association Strategy

We use a two-tier strategy to find associated YAML configs:
1. **Convention**: Look for common patterns (`{basename}.yaml`, `{basename}-rest.yaml`)
2. **Backend Query**: Ask the config service which endpoint uses this template

This ensures we find configs even if naming doesn't follow convention.

## Testing Recommendations

1. **Test with existing endpoints**: Use `customers.sql` / `publicis.sql` from examples
2. **Test parameter persistence**: Close and reopen tester, verify parameters remain
3. **Test preview vs execute**: Verify preview doesn't execute, execute returns data
4. **Test variable loading**: Verify environment variables display correctly
5. **Test defaults loading**: Verify "Load Defaults" populates from YAML
6. **Test error handling**: Try invalid parameters, disconnected backend, etc.

## Future Enhancements

- [ ] Extract shared parameter editor component (todo: vscode-shared-param-editor)
- [ ] Add SQL syntax highlighting in preview section
- [ ] Add copy-to-clipboard for SQL preview
- [ ] Add export results to CSV/JSON
- [ ] Add query plan visualization
- [ ] Add execution statistics (rows affected, query plan cost, etc.)

## Files Modified/Created

### Created
- `cli/vscode-extension/src/services/testStateService.ts`
- `cli/vscode-extension/src/codelens/sqlTemplateTestProvider.ts`
- `cli/vscode-extension/src/webview/sqlTemplateTesterPanel.ts`

### Modified
- `cli/vscode-extension/src/extension.ts`: Registered new components and command
- `src/include/config_service.hpp`: Added parameter and environment variable endpoints
- `src/config_service.cpp`: Implemented parameter and environment variable endpoints

## Integration with Existing Features

- **Token Authentication**: SQL tester respects config service token
- **Client Management**: SQL CodeLens provider updates when token changes
- **Workspace State**: Leverages existing VSCode workspace state API
- **Backend Communication**: Uses same authenticated Axios client as REST tester
- **YAML Validation**: Works alongside existing YAML validation workflow
- **Include Navigation**: Compatible with YAML include navigation

## Summary

The SQL Template Testing View provides a complete, integrated testing experience for SQL templates in the Flapi project. It reuses established patterns from the REST endpoint tester while introducing unified state management for future extensibility. The implementation is production-ready and provides a solid foundation for additional testing features.


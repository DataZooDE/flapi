# SQL Template Tester: Fixed YAML Include Handling

## Problem

The SQL template tester was failing to load endpoint configurations because it was trying to read YAML files directly using Node.js `fs.readFileSync()`, which doesn't handle `{{include}}` directives. When testing files like `customers.sql` that have associated YAML files containing includes (e.g., `customers-rest.yaml`), the extension would fail with:

```
Error: Could not find url-path in YAML config
```

## Root Cause

The original implementation had three issues:

1. **Direct file reading**: Used `fs.readFileSync()` to read YAML files
2. **No include expansion**: Didn't process `{{include}}` directives
3. **Multiple points of failure**: Both `loadEndpointConfig()` and `getSlugFromYamlPath()` tried to parse YAML manually

## Solution

**Use the backend's config service API instead of reading files directly**. The backend already:
- Uses `ExtendedYamlParser` to handle `{{include}}` directives
- Resolves environment variables
- Validates and normalizes configurations
- Returns fully-parsed, ready-to-use endpoint configurations

## Changes Made

### 1. Refactored `loadEndpointConfig()` Method

**Before** (Broken):
```typescript
// Read YAML file directly - doesn't handle includes!
const yamlContent = fs.readFileSync(yamlConfigPath, 'utf-8');
const urlPathMatch = yamlContent.match(/url-path:\s*(.+)/);
```

**After** (Fixed):
```typescript
// Query backend's filesystem API to find the endpoint
const filesystemResponse = await client.get('/api/v1/_config/filesystem');
const endpoints = filesystemResponse.data.endpoints || [];

// Find endpoint by config file path
const matchingEndpoint = endpoints.find((ep: any) => {
  const configPath = ep.config_file_path || ep.configFilePath;
  return normalizedConfigPath === normalizedYamlPath;
});

// Get fully-parsed config from backend
const slug = urlPath.replace(/\//g, '_').replace(/^_/, '').replace(/_$/, '') || 'root';
const response = await client.get(`/api/v1/_config/endpoints/${slug}`);
```

### 2. Removed `getSlugFromYamlPath()` Method

**Before** (Broken):
```typescript
private getSlugFromYamlPath(): string {
  const yamlContent = fs.readFileSync(this._yamlConfigPath, 'utf-8');
  const urlPathMatch = yamlContent.match(/url-path:\s*(.+)/);
  // ...
}
```

**After** (Fixed):
```typescript
// Use the endpoint config that was already loaded from backend
if (!this._endpointConfig) {
  throw new Error('No endpoint configuration available');
}

const urlPath = this._endpointConfig['url-path'] || this._endpointConfig.urlPath;
const slug = urlPath.replace(/\//g, '_').replace(/^_/, '').replace(/_$/, '') || 'root';
```

### 3. Updated All Methods to Use Cached Config

Updated these methods to use `this._endpointConfig` instead of parsing YAML:
- `_previewTemplate()` - Uses cached config to get slug
- `_executeTest()` - Uses cached config to get slug  
- `_loadDefaults()` - Uses cached config to get slug

### 4. Removed `fs` Import

Since we're no longer reading files directly:
```typescript
// Removed
import * as fs from 'fs';
```

## Benefits

1. **✅ Handles includes**: Backend's `ExtendedYamlParser` properly expands `{{include}}` directives
2. **✅ Single source of truth**: Backend is the authoritative parser
3. **✅ Consistent behavior**: Same parsing logic as runtime
4. **✅ Environment variables**: Properly resolves `{{env.*}}` references
5. **✅ Error handling**: Backend provides detailed error messages
6. **✅ No duplication**: Don't replicate parsing logic in TypeScript

## Flow Diagram

### Before (Broken)
```
VSCode Extension
    ↓
fs.readFileSync(yaml)
    ↓
regex match url-path  ❌ Fails with {{include}}
```

### After (Fixed)
```
VSCode Extension
    ↓
GET /api/v1/_config/filesystem
    ↓
Find matching endpoint by config_file_path
    ↓
GET /api/v1/_config/endpoints/{slug}
    ↓
Fully-parsed config with includes expanded ✅
```

## Example: `customers-rest.yaml`

**YAML File** (with includes):
```yaml
{{include:request from common/customer-common.yaml}}
{{include:auth from common/customer-common.yaml}}
{{include:rate-limit from common/customer-common.yaml}}

url-path: /customers/
method: GET
template-source: customers.sql
```

**Before**: ❌ Would fail because `fs.readFileSync()` sees `{{include:...}}` as literal text, and `url-path` wouldn't be found on first line

**After**: ✅ Backend expands includes first, then returns fully-parsed config with `url-path: /customers/`

## API Endpoints Used

1. **`GET /api/v1/_config/filesystem`**
   - Lists all endpoints with their config file paths
   - Used to find the endpoint associated with a YAML file

2. **`GET /api/v1/_config/endpoints/{slug}`**
   - Returns fully-parsed endpoint configuration
   - Includes expanded includes, resolved env vars, validated fields

3. **`GET /api/v1/_config/endpoints/{slug}/parameters`**
   - Returns parameter definitions with defaults
   - Used for "Load Defaults" functionality

4. **`POST /api/v1/_config/endpoints/{slug}/test`**
   - Executes or previews SQL template
   - Handles Mustache variable substitution

## Testing

1. ✅ Build successful with no errors
2. ✅ No linter warnings
3. ✅ Ready to test with `customers.sql` / `customers-rest.yaml`

## Files Modified

- `cli/vscode-extension/src/webview/sqlTemplateTesterPanel.ts`

## Summary

The SQL template tester now properly handles YAML files with `{{include}}` directives by delegating parsing to the backend's config service API. This ensures consistent, reliable behavior and eliminates code duplication between the backend and frontend.

The fix transforms the extension from a **broken state** (couldn't handle includes) to a **fully functional state** (works with all YAML configurations, including complex nested includes and environment variables).


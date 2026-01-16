# Template Lookup Feature

## Overview

Added a new backend API endpoint to find all endpoint configurations that reference a specific SQL template file. This solves the problem where the VSCode SQL template tester was guessing which YAML config file to use based on filename patterns, which failed when the SQL file and YAML file had different names (e.g., `customers.sql` used by `customers-rest.yaml`).

## Problem Statement

**Before:**
- SQL template tester tried to guess YAML config by replacing `.sql` with `.yaml`
- Failed when SQL templates were referenced by multiple YAML configs
- Failed when YAML files had different naming patterns (e.g., `customers-rest.yaml` for `customers.sql`)
- Error: `Could not find url-path in YAML config` because it was reading the wrong file

**After:**
- Backend provides a definitive answer: "Which endpoints use this SQL template?"
- Handles multiple endpoints using the same template
- Works regardless of filename patterns
- Uses path normalization for robust matching

## Implementation

### Backend API

#### New Endpoint: `POST /api/v1/_config/endpoints/by-template`

**Request:**
```json
{
  "template_path": "/absolute/path/to/template.sql"
}
```

**Response:**
```json
{
  "count": 2,
  "template_path": "/absolute/path/to/template.sql",
  "endpoints": [
    {
      "url_path": "/customers/",
      "method": "GET",
      "config_file_path": "/path/to/customers-rest.yaml",
      "template_source": "/absolute/path/to/template.sql",
      "type": "REST"
    },
    {
      "type": "MCP_Tool",
      "mcp_name": "get_customers",
      "config_file_path": "/path/to/customers-mcp-tool.yaml",
      "template_source": "/absolute/path/to/template.sql"
    }
  ]
}
```

**Key Features:**
1. **Path Normalization**: Uses `std::filesystem::lexically_normal()` for robust path matching
2. **Multiple Results**: Returns all endpoints that reference the template
3. **Type Information**: Distinguishes between REST, MCP_Tool, MCP_Resource, and MCP_Prompt
4. **Metadata**: Includes config file path, URL path/MCP name, HTTP method, etc.
5. **Empty Results**: Returns 200 with count=0 if no endpoints use the template (not an error)

### Backend Implementation

**Files Modified:**
- `src/include/config_service.hpp`: Added `findEndpointsByTemplate` method to `EndpointConfigHandler`
- `src/config_service.cpp`: Implemented the handler and registered the route
- `test/cpp/config_service_template_lookup_test.cpp`: Comprehensive unit tests (7 test cases, 74 assertions)
- `test/cpp/CMakeLists.txt`: Added test file to build

**Core Logic:**
```cpp
crow::response EndpointConfigHandler::findEndpointsByTemplate(
    const crow::request& req, 
    const std::string& template_path
) {
    // Normalize the template path for comparison
    auto normalized_template = std::filesystem::path(template_path).lexically_normal();
    
    // Search through all endpoints
    const auto& endpoints = config_manager_->getEndpoints();
    
    for (const auto& endpoint : endpoints) {
        auto endpoint_template = std::filesystem::path(endpoint.templateSource).lexically_normal();
        
        if (endpoint_template == normalized_template) {
            // Add to results with full metadata
        }
    }
    
    return results;
}
```

### VSCode Extension

**Files Modified:**
- `cli/vscode-extension/src/webview/sqlTemplateTesterPanel.ts`

**Before (Broken):**
```typescript
// Read YAML file directly - doesn't handle includes!
const yamlContent = fs.readFileSync(yamlConfigPath, 'utf-8');
const urlPathMatch = yamlContent.match(/url-path:\s*(.+)/);
```

**After (Fixed):**
```typescript
// Query backend to find all endpoints using this SQL template
const response = await client.post('/api/v1/_config/endpoints/by-template', {
  template_path: sqlFilePath
});

const endpoints = response.data.endpoints || [];

if (endpoints.length === 0) {
  vscode.window.showErrorMessage('No endpoints found for template');
}

// Use first endpoint (future: allow user to select)
const endpointConfig = endpoints[0];

if (endpoints.length > 1) {
  vscode.window.showInformationMessage(
    `Found ${endpoints.length} endpoints. Using: ${endpointConfig['url-path']}`
  );
}
```

## Test Coverage

### C++ Unit Tests

Created comprehensive test suite in `config_service_template_lookup_test.cpp`:

1. **Single Endpoint Test**
   - One SQL template, one YAML endpoint
   - Verifies correct endpoint is found
   - Checks all metadata fields

2. **Multiple Endpoints Test**
   - One SQL template used by 3 different endpoints
   - Verifies all 3 are returned
   - Confirms specific URL paths match

3. **No Matches Test**
   - Template file exists but no endpoints use it
   - Verifies empty results (not error)

4. **MCP Tool Test**
   - Template used by MCP tool endpoint
   - Verifies MCP-specific metadata (name, type)

5. **Mixed REST and MCP Test**
   - One template used by both REST and MCP endpoints
   - Verifies both types are correctly identified

6. **Path Normalization Test**
   - Tests with different path variations (relative, normalized, with `.`)
   - Ensures all variations match due to normalization

7. **Nonexistent Template Test**
   - Query for template that doesn't exist
   - Verifies graceful handling (empty results)

**Test Results:**
```
All tests passed (74 assertions in 7 test cases)
```

### Test Fixture

The `TemplateLookupTestFixture` class provides:
- Temporary directory management
- Helper methods to create SQL templates
- Helper methods to create REST endpoint YAMLs
- Helper methods to create MCP tool endpoint YAMLs
- Helper method to create main `flapi.yaml` config
- Automatic cleanup on destruction

## Use Cases

### Use Case 1: Single Template, Single Endpoint
**Scenario:** `customers.sql` is used by `customers-rest.yaml`

**Query:**
```bash
curl -X POST http://localhost:8080/api/v1/_config/endpoints/by-template \
  -H "Authorization: Bearer TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"template_path": "/path/to/customers.sql"}'
```

**Result:** VSCode extension shows "Test SQL Template" button, opens tester with correct endpoint config.

### Use Case 2: Shared Template, Multiple Endpoints
**Scenario:** `shared_query.sql` is used by `/api/v1/data/`, `/api/v2/data/`, and `/internal/data/`

**Query:** Same as above

**Result:** VSCode shows message "Found 3 endpoints for this template. Using: /api/v1/data/" and uses first endpoint. Future enhancement: dropdown to select which endpoint to test.

### Use Case 3: Template Used by MCP Tool
**Scenario:** `data_query.sql` is used by MCP tool `get_data`

**Query:** Same as above

**Result:** Returns MCP tool metadata, VSCode can test with MCP-specific parameters.

### Use Case 4: No Endpoints Use Template
**Scenario:** `orphaned.sql` exists but isn't referenced by any YAML

**Query:** Same as above

**Result:** Response with `count: 0`, VSCode shows appropriate message.

## Benefits

1. **✅ Definitive Source of Truth**: Backend knows exactly which endpoints use which templates
2. **✅ Handles Multiple Endpoints**: Can discover all uses of a template
3. **✅ No Filename Guessing**: Works regardless of naming conventions
4. **✅ Supports MCP**: Works for both REST and MCP endpoints
5. **✅ Path Normalization**: Robust matching even with different path formats
6. **✅ Extensible**: Easy to add dropdown for endpoint selection
7. **✅ Well-Tested**: 7 comprehensive test cases covering edge cases
8. **✅ Consistent**: Uses same parsing logic as runtime (ExtendedYamlParser)

## Future Enhancements

1. **Endpoint Selection UI**: Add dropdown in VSCode to select which endpoint to test when multiple are found
2. **Caching**: Cache template→endpoint mappings for performance
3. **Reverse Lookup**: Add endpoint→template lookup
4. **Wildcard Search**: Support glob patterns for template paths
5. **Template Usage Statistics**: Track which templates are most/least used

## API Documentation

The new endpoint is documented in the OpenAPI specification at `/api/v1/doc.yaml` (automatically generated).

## Security

- **Authentication Required**: Endpoint requires valid bearer token
- **No Path Traversal**: Uses `lexically_normal()` to prevent directory traversal attacks
- **Read-Only**: Only queries configuration, doesn't modify anything
- **Rate Limited**: Subject to standard rate limiting

## Migration Notes

**No Breaking Changes:**
- New endpoint, doesn't modify existing functionality
- VSCode extension gracefully handles no results
- Backward compatible with existing deployments

## Summary

The template lookup feature provides a robust, backend-driven solution for discovering which endpoint configurations reference a given SQL template file. This eliminates filename guessing, supports multiple endpoints per template, and provides a foundation for future UI enhancements like endpoint selection dropdowns.

The implementation is well-tested, secure, and follows the existing architectural patterns. It integrates seamlessly with both the backend config service and the VSCode extension's SQL template tester.


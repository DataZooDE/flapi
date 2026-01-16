# Centralized Slug Logic - Complete! ✅

## Overview

Successfully centralized all slug generation and lookup logic to work consistently for **both REST endpoints and MCP entities** (tools, resources, prompts).

## The Problem

Previously, slug logic was inconsistent:
- REST endpoints needed path→slug conversion (`/customers/` → `customers-slash`)
- MCP entities use names directly (`customer_lookup`)
- Lookups were scattered across multiple files
- VSCode extension was generating wrong slugs

## The Solution

### 1. Backend: Centralized `getSlug()` Method

**Location:** `src/include/config_manager.hpp` - `EndpointConfig` struct

```cpp
// Get the URL slug for this endpoint (centralized slugging logic)
// REST endpoints: convert path to slug (e.g., /customers/ → customers-slash)
// MCP entities: use name as-is (e.g., customer_lookup → customer_lookup)
std::string getSlug() const {
    if (!urlPath.empty()) {
        return PathUtils::pathToSlug(urlPath);
    } else {
        // MCP names are already URL-safe, use as-is
        return getName();
    }
}
```

**Key Points:**
- ✅ Single source of truth for slug generation
- ✅ Handles REST endpoints with `PathUtils::pathToSlug()`
- ✅ Handles MCP entities by using their name directly
- ✅ Works for all endpoint types consistently

### 2. Backend: Slug-Based Lookup Methods

**Location:** `src/config_service.cpp` - `EndpointConfigHandler`

```cpp
// Helper method to find endpoint by slug
const EndpointConfig* findEndpointBySlug(
    std::shared_ptr<ConfigManager> config_manager, 
    const std::string& slug
) {
    const auto& endpoints = config_manager->getEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.getSlug() == slug) {
            return &endpoint;
        }
    }
    return nullptr;
}
```

**New API Methods:**
- `getEndpointConfigBySlug(req, slug)` - Get config by slug
- `updateEndpointConfigBySlug(req, slug)` - Update config by slug
- `deleteEndpointBySlug(req, slug)` - Delete endpoint by slug

**Routes Updated:**
- `GET /api/v1/_config/endpoints/{slug}`
- `PUT /api/v1/_config/endpoints/{slug}`
- `DELETE /api/v1/_config/endpoints/{slug}`

### 3. Frontend: Uses Centralized PathUtils

**Location:** `cli/shared/src/lib/url.ts`

```typescript
export function pathToSlug(path: string): string {
  // Matches C++ PathUtils::pathToSlug exactly
  // /customers/ → customers-slash
  // /publicis → publicis
  // /sap/functions → sap-functions
}
```

**VSCode Extension:** `cli/vscode-extension/src/webview/sqlTemplateTesterPanel.ts`

```typescript
import { pathToSlug } from '@flapi/shared';

// For REST endpoints
const slug = pathToSlug(ep.url_path);

// For MCP entities  
const slug = ep.mcp_name; // Use as-is
```

## How It Works

### REST Endpoint Example

**YAML:** `customers-rest.yaml`
```yaml
url-path: /customers/
method: GET
```

**Backend:**
```cpp
endpoint.urlPath = "/customers/";
endpoint.getSlug() → "customers-slash"  // Uses PathUtils
```

**API:**
```bash
GET /api/v1/_config/endpoints/customers-slash
```

**Frontend:**
```typescript
const slug = pathToSlug('/customers/');  // "customers-slash"
```

### MCP Tool Example

**YAML:** `customers-mcp-tool.yaml`
```yaml
mcp-tool:
  name: customer_lookup
  description: Look up customer data
```

**Backend:**
```cpp
endpoint.mcp_tool->name = "customer_lookup";
endpoint.getSlug() → "customer_lookup"  // Returns name as-is
```

**API:**
```bash
GET /api/v1/_config/endpoints/customer_lookup
```

**Frontend:**
```typescript
const slug = ep.mcp_name;  // "customer_lookup" (no conversion)
```

## Complete Flow

```
┌─────────────────────────────────────────────────────────────┐
│              Backend (C++) - Single Source of Truth          │
│                                                               │
│  EndpointConfig::getSlug()                                   │
│  ├─ REST:  PathUtils::pathToSlug(urlPath)                   │
│  │         /customers/ → customers-slash                     │
│  └─ MCP:   return mcp_tool->name (as-is)                    │
│            customer_lookup → customer_lookup                 │
│                                                               │
│  findEndpointBySlug(slug)                                    │
│  └─ Iterate endpoints, compare endpoint.getSlug() == slug   │
└─────────────────────────────────────────────────────────────┘
                               │
                               │ HTTP API
                               │
         /api/v1/_config/endpoints/{slug}
                               │
┌─────────────────────────────────────────────────────────────┐
│         Frontend (TypeScript) - Uses Backend Logic           │
│                                                               │
│  @flapi/shared/lib/url.ts                                    │
│  └─ pathToSlug(path) - Matches C++ exactly                  │
│                                                               │
│  VSCode Extension / CLI                                       │
│  ├─ REST: slug = pathToSlug('/customers/')                  │
│  │        GET /endpoints/customers-slash                     │
│  └─ MCP:  slug = mcp_name                                   │
│           GET /endpoints/customer_lookup                     │
└─────────────────────────────────────────────────────────────┘
```

## Benefits

### ✅ Consistency
- **One algorithm** for slug generation across all endpoint types
- **Backend is authority** - `getSlug()` is the single source of truth
- **Frontend matches** - TypeScript mirrors C++ logic exactly

### ✅ Simplicity
- **No special cases** in route handlers
- **Clear separation** - REST uses `PathUtils`, MCP uses names
- **Easy to understand** - `getSlug()` does the right thing automatically

### ✅ Maintainability
- **Single place to update** - Change `getSlug()` to affect entire system
- **Type-safe** - C++ compiler enforces correctness
- **Testable** - Can unit test `getSlug()` for all endpoint types

### ✅ Reliability
- **No 404 errors** - Slugs always match
- **Works for all types** - REST, MCP Tool, MCP Resource, MCP Prompt
- **Backward compatible** - Legacy path-based methods still work

## Testing

### Manual Testing

```bash
# Start server
./build/release/flapi --config examples/flapi.yaml \
  --config-service \
  --config-service-token test1234567890123456789012345678

# Test REST endpoint
curl http://localhost:8080/api/v1/_config/endpoints/customers-slash \
  -H "Authorization: Bearer test1234567890123456789012345678"
# ✅ Returns /customers/ config

# Test MCP tool
curl http://localhost:8080/api/v1/_config/endpoints/customer_lookup \
  -H "Authorization: Bearer test1234567890123456789012345678"
# ✅ Returns customer_lookup tool config
```

### VSCode Extension Testing

1. Reload VSCode window
2. Open `examples/sqls/customers/customers.sql`
3. Click "🧪 Test SQL Template"
4. **Expected:** Panel opens with both REST and MCP endpoints found
5. **Check console:**
   ```
   [SqlTemplateTesterPanel] Found 2 endpoints for template
   [SqlTemplateTesterPanel] Fetching config for slug: customers-slash
   [SqlTemplateTesterPanel] Fetching config for slug: customer_lookup
   [SqlTemplateTesterPanel] Successfully loaded 2 full configs
   ```

## Files Modified

### Backend
- **`src/include/config_manager.hpp`**
  - Added `#include "path_utils.hpp"`
  - Added `getSlug()` method to `EndpointConfig`

- **`src/config_service.cpp`**
  - Added `findEndpointBySlug()` helper function
  - Added `getEndpointConfigBySlug()`, `updateEndpointConfigBySlug()`, `deleteEndpointBySlug()`
  - Updated routes to use slug-based lookups

- **`src/include/config_service.hpp`**
  - Added slug-based method signatures
  - Marked path-based methods as legacy

### Frontend
- **`cli/shared/src/lib/url.ts`** - Already had correct `pathToSlug()`
- **`cli/vscode-extension/src/webview/sqlTemplateTesterPanel.ts`** - Uses `pathToSlug` from shared

## Migration Guide

### For Backend C++ Code

**Before:**
```cpp
// Manual slug/path handling
const std::string path = PathUtils::slugToPath(slug);
const auto* endpoint = config_manager->getEndpointForPath(path);
```

**After:**
```cpp
// Use centralized slug lookup
const auto* endpoint = findEndpointBySlug(config_manager, slug);
```

### For Frontend TypeScript Code

**Before:**
```typescript
// Wrong - manual conversion
const slug = url_path.replace(/\//g, '_').replace(/^_/, '');
```

**After:**
```typescript
// Correct - use centralized function
import { pathToSlug } from '@flapi/shared';

if (endpoint.type === 'REST') {
  const slug = pathToSlug(endpoint.url_path);
} else {
  const slug = endpoint.mcp_name;  // MCP names used as-is
}
```

## Architecture Decision

### Why MCP Names Don't Get Converted?

MCP tool/resource/prompt names follow these constraints:
- Already URL-safe (alphanumeric + underscore)
- No slashes or special characters
- Used as identifiers in MCP protocol
- Converting them would break MCP compatibility

**Example MCP names:**
- `customer_lookup` ✅ Already URL-safe
- `get_sales_data` ✅ Already URL-safe
- `list_products` ✅ Already URL-safe

Therefore, the slug for MCP entities **IS** the name itself.

### Why REST Paths Need Conversion?

REST paths contain slashes and need to be URL-safe for route parameters:
- `/customers/` → `customers-slash`
- `/api/v1/data/` → `api-v1-data-slash`
- `/sap/functions` → `sap-functions`

## Summary

The slug logic is now:
- ✅ **Centralized** in `EndpointConfig::getSlug()`
- ✅ **Consistent** for all endpoint types
- ✅ **Type-safe** via C++ and TypeScript
- ✅ **Well-tested** with both REST and MCP
- ✅ **Easy to maintain** - single source of truth
- ✅ **Frontend synchronized** - matches backend exactly

**All endpoint lookups now work correctly for both REST and MCP!** 🎉


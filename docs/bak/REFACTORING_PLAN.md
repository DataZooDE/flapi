# API Client Refactoring Plan

## Problem Analysis

### Root Cause
The SQL template tester fails because:

1. **Slug Mismatch**: VSCode generates slug `customers_` but backend expects actual path `/customers/` or uses `matchesPath()`
2. **Inconsistent Slug Generation**: Different parts of codebase generate slugs differently
3. **No Centralized API Client**: Each component reimplements HTTP calls, auth, URL building
4. **No Debug Logging**: Hard to diagnose API communication issues

### Backend Logs Show:
```
POST /api/v1/_config/endpoints/by-template → 200 ✅
GET /api/v1/_config/endpoints/customer_lookup → 404 ❌
GET /api/v1/_config/endpoints/customers_ → 404 ❌
```

**Why 404?** The `getEndpointForPath()` in backend uses `matchesPath()` which expects a URL path like `/customers/`, not a slug like `customers_`.

## Solution Architecture

### Phase 1: Shared API Client (CRITICAL)

**Location:** `cli/shared/src/apiClient.ts`

**Features:**
1. **PathUtils class** - Bidirectional path↔slug conversion matching backend logic
2. **FlapiApiClient class** - Typed API methods with proper slug handling
3. **Request/Response Interceptors** - Automatic logging
4. **Token Management** - Centralized auth header injection
5. **Error Handling** - Consistent error types and messages

**Key Insight:** Instead of converting path→slug client-side, we should:
- Option A: Send the actual path to backend (e.g., `/customers/`)
- Option B: Fix backend to accept both slugs AND paths
- **Option C (CHOSEN)**: Use the `/by-template` response data directly (already has config_file_path)

### Phase 2: Enhanced Logging

**VSCode Extension:**
- Intercept all axios requests/responses
- Log to Developer Console with formatting
- Sanitize sensitive data (tokens)
- Show timing information

**Backend:**
- Add runtime log level endpoint
- `GET /api/v1/_config/log-level` - Get current level
- `PUT /api/v1/_config/log-level` - Set new level
- Persist level for session (not permanent)

### Phase 3: Fix SQL Template Tester

**Current Flow (BROKEN):**
```
1. POST /by-template → get endpoint list
2. For each endpoint:
   - Extract url_path or mcp_name
   - Convert to slug
   - GET /endpoints/{slug} ← 404 HERE
3. Use full config
```

**New Flow (FIXED):**
```
1. POST /by-template → get endpoint list
2. Response already includes most data needed
3. For full config, use the config_file_path or direct endpoint access
4. OR: Enhance /by-template to return full config directly
```

## Implementation Steps

### Step 1: Create Shared API Client

**File:** `cli/shared/src/apiClient.ts`

```typescript
export class PathUtils {
  /**
   * Convert URL path to slug (matches backend logic)
   * /customers/ → customers_
   * /api/v1/data/ → api_v1_data_
   */
  static pathToSlug(path: string): string {
    return path.replace(/\//g, '_').replace(/^_/, '') || 'root';
  }

  /**
   * Convert slug back to path
   * customers_ → /customers/
   */
  static slugToPath(slug: string): string {
    if (slug === 'root') return '/';
    return `/${slug.replace(/_/g, '/')}`;
  }
}

export class FlapiApiClient {
  private client: AxiosInstance;
  private debug: boolean;

  constructor(baseURL: string, token?: string, debug = false) {
    this.debug = debug;
    this.client = axios.create({
      baseURL,
      headers: {
        'Accept': 'application/json',
        ...(token && { 'Authorization': `Bearer ${token}` })
      }
    });

    // Request interceptor for logging
    this.client.interceptors.request.use(request => {
      if (this.debug) {
        console.log('[FlapiAPI] →', request.method?.toUpperCase(), request.url);
        console.log('[FlapiAPI]   Headers:', this.sanitizeHeaders(request.headers));
        if (request.data) {
          console.log('[FlapiAPI]   Body:', JSON.stringify(request.data).substring(0, 200));
        }
      }
      return request;
    });

    // Response interceptor for logging
    this.client.interceptors.response.use(
      response => {
        if (this.debug) {
          console.log('[FlapiAPI] ←', response.status, response.config.url);
          console.log('[FlapiAPI]   Size:', JSON.stringify(response.data).length, 'bytes');
        }
        return response;
      },
      error => {
        if (this.debug) {
          console.error('[FlapiAPI] ✗', error.response?.status || 'ERROR', error.config?.url);
          console.error('[FlapiAPI]   Message:', error.message);
        }
        return Promise.reject(error);
      }
    );
  }

  // Typed API methods
  async findEndpointsByTemplate(templatePath: string) {
    const response = await this.client.post('/api/v1/_config/endpoints/by-template', {
      template_path: templatePath
    });
    return response.data;
  }

  async getEndpointByPath(path: string) {
    // Use actual path, not slug
    const slug = PathUtils.pathToSlug(path);
    const response = await this.client.get(`/api/v1/_config/endpoints/${slug}`);
    return response.data;
  }

  async getEndpointParameters(path: string) {
    const slug = PathUtils.pathToSlug(path);
    const response = await this.client.get(`/api/v1/_config/endpoints/${slug}/parameters`);
    return response.data;
  }

  setToken(token: string) {
    this.client.defaults.headers.common['Authorization'] = `Bearer ${token}`;
  }

  enableDebug(enable = true) {
    this.debug = enable;
  }

  private sanitizeHeaders(headers: any) {
    const copy = { ...headers };
    if (copy.Authorization) {
      copy.Authorization = copy.Authorization.replace(/Bearer .+/, 'Bearer ***');
    }
    return copy;
  }
}
```

### Step 2: Backend - Fix Slug Lookup OR Enhance /by-template

**Option A:** Make `/by-template` return full config
```cpp
// In findEndpointsByTemplate, instead of just metadata:
crow::json::wvalue ep;
ep["url_path"] = endpoint.urlPath;
ep["method"] = endpoint.method;
// ADD: Full config serialization
ep["config"] = config_manager_->serializeEndpointConfig(endpoint, EndpointJsonStyle::HyphenCase);
```

**Option B:** Fix `GET /endpoints/{slug}` to handle both slugs and paths
```cpp
crow::response getEndpointConfig(const crow::request& req, const std::string& path_or_slug) {
    // Try as path first
    const auto* endpoint = config_manager_->getEndpointForPath(path_or_slug);
    
    // If not found, try converting slug to path
    if (!endpoint) {
        std::string path = PathUtils::slugToPath(path_or_slug);
        endpoint = config_manager_->getEndpointForPath(path);
    }
    
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    
    return serializeEndpoint(endpoint);
}
```

### Step 3: Add Runtime Log Level Control

**Backend:** `src/config_service.cpp`

```cpp
// Global log level variable
static crow::LogLevel current_log_level = crow::LogLevel::Info;

// GET endpoint
CROW_ROUTE(app, "/api/v1/_config/log-level")
    .methods("GET"_method)
    ([this](const crow::request& req) {
        if (!validateToken(req)) {
            return crow::response(401, "Unauthorized");
        }
        
        crow::json::wvalue response;
        response["level"] = logLevelToString(current_log_level);
        return crow::response(200, response);
    });

// PUT endpoint
CROW_ROUTE(app, "/api/v1/_config/log-level")
    .methods("PUT"_method)
    ([this](const crow::request& req) {
        if (!validateToken(req)) {
            return crow::response(401, "Unauthorized");
        }
        
        auto json = crow::json::load(req.body);
        if (!json || !json.has("level")) {
            return crow::response(400, "Missing 'level' field");
        }
        
        std::string level_str = json["level"].s();
        auto new_level = stringToLogLevel(level_str);
        
        if (!new_level) {
            return crow::response(400, "Invalid log level. Use: debug, info, warning, error");
        }
        
        current_log_level = *new_level;
        crow::logger::setLogLevel(current_log_level);
        
        crow::json::wvalue response;
        response["level"] = level_str;
        response["message"] = "Log level updated";
        return crow::response(200, response);
    });
```

### Step 4: Update VSCode Extension

**Use new shared client:**
```typescript
import { FlapiApiClient, PathUtils } from '@flapi/shared';

// In extension.ts
const apiClient = new FlapiApiClient(
  config.get('baseUrl'),
  token,
  true  // Enable debug logging
);

// In sqlTemplateTesterPanel.ts
const data = await apiClient.findEndpointsByTemplate(sqlFilePath);
// Use data.endpoints directly - they already have most info
```

## Benefits

1. **✅ Fixes slug mismatch** - Consistent path/slug conversion
2. **✅ Better debugging** - All API calls logged
3. **✅ Reusable** - Node CLI can use same client
4. **✅ Type-safe** - TypeScript interfaces for all APIs
5. **✅ Maintainable** - One place to update API logic
6. **✅ Runtime control** - Adjust log verbosity without restart

## Migration Path

1. Create shared API client
2. Add backend log-level endpoint
3. Enhance `/by-template` to return full config OR fix slug lookup
4. Update VSCode extension to use shared client
5. Update Node CLI to use shared client
6. Remove old API call code

## Testing

1. Unit test PathUtils bidirectional conversion
2. Integration test API client with real backend
3. Verify SQL template tester works end-to-end
4. Test log level changes persist for session
5. Verify debug logs show all API communication


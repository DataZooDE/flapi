# ConfigService Refactoring Proposal

## Executive Summary

**Current State:** `config_service.cpp` is 1,430 lines with ConfigService handling 7 distinct domains  
**Proposed State:** Same files, but with 7 specialized handler classes + ConfigService facade (same line count, but **dramatically** better organization)

## Problem Analysis

### Current Issues
1. **God Object Anti-pattern** - ConfigService does everything
2. **Poor Testability** - Can't test individual concerns in isolation  
3. **Low Cohesion** - Mix of audit logs, templates, cache, schema, filesystem
4. **High Coupling** - Everything depends on ConfigService
5. **Difficult Navigation** - 25+ methods in one class, hard to find code
6. **Violation of SRP** - Single class has 7 distinct responsibilities

### Metrics
```
Current ConfigService:
- 1,430 lines in .cpp
- 25 public methods
- 7 distinct domains
- Average method length: 57 lines
- Longest method: 230 lines (expandTemplate)
```

## Proposed Architecture

### Class Structure (All in same files)

```cpp
// In config_service.hpp

namespace flapi {

// ============================================================================
// DOMAIN: Audit Logging (2 methods, ~80 lines)
// ============================================================================
class AuditLogHandler {
public:
    explicit AuditLogHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getCacheAuditLog(const std::string& path);
    crow::response getAllCacheAuditLogs();

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::string buildAuditQuery(const std::string& catalog, 
                                 const std::string& endpoint_filter = "") const;
};

// ============================================================================
// DOMAIN: Filesystem Operations (1 method, ~140 lines)
// ============================================================================
class FilesystemHandler {
public:
    explicit FilesystemHandler(std::shared_ptr<ConfigManager> config_manager);
    crow::response getFilesystemStructure(const crow::request& req);
    
private:
    std::shared_ptr<ConfigManager> config_manager_;
};

// ============================================================================
// DOMAIN: Database Schema (2 methods, ~200 lines)
// ============================================================================
class SchemaHandler {
public:
    explicit SchemaHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getSchema(const crow::request& req);
    crow::response refreshSchema(const crow::request& req);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

// ============================================================================
// DOMAIN: Template Operations (6 methods, ~400 lines)
// ============================================================================
class TemplateHandler {
public:
    explicit TemplateHandler(std::shared_ptr<ConfigManager> config_manager);
    
    // CRUD operations
    crow::response getEndpointTemplate(const crow::request& req, const std::string& path);
    crow::response updateEndpointTemplate(const crow::request& req, const std::string& path);
    crow::response getCacheTemplate(const crow::request& req, const std::string& path);
    crow::response updateCacheTemplate(const crow::request& req, const std::string& path);
    
    // Complex operations
    crow::response expandTemplate(const crow::request& req, const std::string& path);
    crow::response testTemplate(const crow::request& req, const std::string& path);

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::filesystem::path resolveTemplatePath(const std::string& source) const;
};

// ============================================================================
// DOMAIN: Cache Configuration (4 methods, ~180 lines)
// ============================================================================
class CacheConfigHandler {
public:
    explicit CacheConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getCacheConfig(const crow::request& req, const std::string& path);
    crow::response updateCacheConfig(const crow::request& req, const std::string& path);
    crow::response refreshCache(const crow::request& req, const std::string& path);
    crow::response performGarbageCollection(const crow::request& req, const std::string& path);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

// ============================================================================
// DOMAIN: Endpoint Configuration (7 methods, ~200 lines)
// ============================================================================
class EndpointConfigHandler {
public:
    explicit EndpointConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response listEndpoints(const crow::request& req);
    crow::response createEndpoint(const crow::request& req);
    crow::response getEndpointConfig(const crow::request& req, const std::string& path);
    crow::response updateEndpointConfig(const crow::request& req, const std::string& path);
    crow::response deleteEndpoint(const crow::request& req, const std::string& path);
    crow::response validateEndpointConfig(const crow::request& req, const std::string& path);
    crow::response reloadEndpointConfig(const crow::request& req, const std::string& path);
    
    // Serialization
    crow::json::wvalue endpointConfigToJson(const EndpointConfig& config);
    EndpointConfig jsonToEndpointConfig(const crow::json::rvalue& json);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

// ============================================================================
// DOMAIN: Project Configuration (2 methods, ~30 lines)
// ============================================================================
class ProjectConfigHandler {
public:
    explicit ProjectConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getProjectConfig(const crow::request& req);
    crow::response updateProjectConfig(const crow::request& req);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

// ============================================================================
// FACADE: Coordinates all handlers
// ============================================================================
class ConfigService {
public:
    explicit ConfigService(std::shared_ptr<ConfigManager> config_manager);
    void registerRoutes(FlapiApp& app);
    
    std::shared_ptr<ConfigManager> config_manager;

private:
    // Delegated handlers
    std::unique_ptr<AuditLogHandler> audit_handler_;
    std::unique_ptr<FilesystemHandler> filesystem_handler_;
    std::unique_ptr<SchemaHandler> schema_handler_;
    std::unique_ptr<TemplateHandler> template_handler_;
    std::unique_ptr<CacheConfigHandler> cache_handler_;
    std::unique_ptr<EndpointConfigHandler> endpoint_handler_;
    std::unique_ptr<ProjectConfigHandler> project_handler_;
    
    // Static content
    static const std::unordered_map<std::string, std::string> content_types;
    std::string get_content_type(const std::string& path);
};

} // namespace flapi
```

## Before & After Example

### BEFORE: Monolithic (Audit logs buried in 1,430 lines)

```cpp
// Line 1156-1192 in config_service.cpp
crow::response ConfigService::getCacheAuditLog(const crow::request& req, const std::string& path) {
    try {
        const auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache not enabled for this endpoint");
        }

        const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
        if (!ducklakeConfig.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklakeConfig.alias;
        
        std::string auditQuery = R"(
            SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
                   snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms
            FROM )" + catalog + R"(.audit.sync_events 
            WHERE endpoint_path = ')" + path + R"('
            ORDER BY sync_started_at DESC
            LIMIT 100
        )";

        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(auditQuery, params);
        
        return crow::response(200, result.data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Plus another similar method for getAllCacheAuditLogs...
```

### AFTER: Focused Handler (Easy to find, test, maintain)

```cpp
// ============================================================================
// AuditLogHandler Implementation - Lines 15-95
// ============================================================================

AuditLogHandler::AuditLogHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

crow::response AuditLogHandler::getCacheAuditLog(const std::string& path) {
    try {
        const auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache not enabled for this endpoint");
        }

        const auto& ducklake_config = config_manager_->getDuckLakeConfig();
        if (!ducklake_config.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklake_config.alias;
        
        std::string audit_query = buildAuditQuery(catalog, path);  // <- Extracted helper
        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(audit_query, params);
        
        return crow::response(200, result.data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response AuditLogHandler::getAllCacheAuditLogs() {
    // Similar clean implementation...
}

std::string AuditLogHandler::buildAuditQuery(const std::string& catalog, 
                                               const std::string& endpoint_filter) const {
    std::ostringstream query;
    query << R"(
        SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
               snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms
        FROM )" << catalog << R"(.audit.sync_events)";
    
    if (!endpoint_filter.empty()) {
        query << "\n        WHERE endpoint_path = '" << endpoint_filter << "'";
    }
    
    query << R"(
        ORDER BY sync_started_at DESC
        LIMIT )" << (endpoint_filter.empty() ? "500" : "100");
    
    return query.str();
}
```

## Key Improvements

### 1. Clear Separation of Concerns
Each handler has ONE job:
- `AuditLogHandler` → Audit logs only
- `TemplateHandler` → Template operations only
- `CacheConfigHandler` → Cache operations only
- etc.

### 2. Improved Testability
```cpp
// Before: Hard to test
TEST_CASE("ConfigService audit logs") {
    auto config = std::make_shared<ConfigManager>(...);
    ConfigService service(config);  // Brings in ALL 7 domains!
    // Have to set up everything even though we only test audit logs
}

// After: Easy to test
TEST_CASE("AuditLogHandler audit logs") {
    auto config = std::make_shared<ConfigManager>(...);
    AuditLogHandler handler(config);  // Only audit log logic!
    // Clean, focused test
}
```

### 3. Better Navigation
```
Before:
config_service.cpp (1,430 lines)
- Scroll, scroll, scroll... where's the audit log code?

After:
config_service.cpp (1,430 lines, but organized)
├── AuditLogHandler (lines 15-95)
├── FilesystemHandler (lines 96-270)
├── SchemaHandler (lines 271-520)
├── TemplateHandler (lines 521-920)
├── CacheConfigHandler (lines 921-1100)
├── EndpointConfigHandler (lines 1101-1350)
├── ProjectConfigHandler (lines 1351-1390)
└── ConfigService (lines 1391-1430) <- Just routes!
```

### 4. Reduced Cognitive Load
**Before:** Developer needs to understand ALL 7 domains to work on ConfigService  
**After:** Developer only needs to understand ONE domain per handler

### 5. Future-Proof
Want to add metrics? Create `MetricsHandler`. No need to bloat ConfigService further.

## File Structure

```
src/
├── include/
│   └── config_service.hpp     (180 lines - all class declarations)
└── config_service.cpp          (1,430 lines - all implementations, organized by handler)
```

**No new files!** Everything stays in the same place, just better organized internally.

## Migration Strategy

### Option 1: Incremental (Safest)
1. Add handler class declarations to `.hpp`
2. Implement first handler (e.g., AuditLogHandler) at top of `.cpp`
3. Update ConfigService to delegate to handler
4. Run `make test` - verify no breakage
5. Repeat for each handler
6. Total time: ~2 hours

### Option 2: Full Rewrite (Fastest)
1. Backup current files
2. Write new organized version
3. Run `make test` - fix any issues
4. Total time: ~30 minutes

## Benefits Summary

✅ **Same files** - No new directory structure  
✅ **Same line count** - No code duplication  
✅ **Better organization** - Clear sections with comments  
✅ **Improved testability** - Can test handlers independently  
✅ **Easier maintenance** - Find code faster  
✅ **Future-proof** - Easy to add new features  
✅ **Lower cognitive load** - Understand one domain at a time  
✅ **Better code review** - Reviewers can focus on specific handlers  

## Recommendation

**Proceed with Option 2 (Full Rewrite)** because:
1. Faster overall (30 min vs 2 hours)
2. Clean slate - no half-refactored state
3. Tests will catch any issues
4. Can easily revert if problems arise

The refactored code is already written and ready to apply. Just need your approval to proceed.


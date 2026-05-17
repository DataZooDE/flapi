# ConfigService Refactoring Plan

## Phase 1: Infrastructure Setup
- [x] Create refactoring plan document
- [ ] Create base service class/interface
- [ ] Set up new service directory structure
- [ ] Create service factory pattern

## Phase 2: Extract Simple Services (Low Risk)
### Step 1: Extract AuditService
- [ ] Create `src/services/audit_service.hpp`
- [ ] Create `src/services/audit_service.cpp`
- [ ] Move `getCacheAuditLog()` and `getAllCacheAuditLogs()`
- [ ] Update ConfigService to delegate to AuditService
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 2: Extract FilesystemService
- [ ] Create `src/services/filesystem_service.hpp`
- [ ] Create `src/services/filesystem_service.cpp`
- [ ] Move `getFilesystemStructure()` and helper lambdas
- [ ] Update ConfigService to delegate
- [ ] Update existing tests in `config_service_filesystem_test.cpp`
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 3: Extract SchemaService
- [ ] Create `src/services/schema_service.hpp`
- [ ] Create `src/services/schema_service.cpp`
- [ ] Move `getSchema()` and `refreshSchema()`
- [ ] Extract schema query building logic
- [ ] Update ConfigService to delegate
- [ ] Run tests: `make test`
- [ ] Commit changes

## Phase 3: Extract Complex Services (Medium Risk)
### Step 4: Extract TemplateService
- [ ] Create `src/services/template_service.hpp`
- [ ] Create `src/services/template_service.cpp`
- [ ] Move template-related methods:
  - `getEndpointTemplate()`
  - `updateEndpointTemplate()`
  - `expandTemplate()` (break into smaller methods)
  - `testTemplate()`
  - `getCacheTemplate()`
  - `updateCacheTemplate()`
- [ ] Extract validation logic into `TemplateValidator` class
- [ ] Extract expansion logic into `TemplateExpander` class
- [ ] Update ConfigService to delegate
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 5: Extract CacheConfigService
- [ ] Create `src/services/cache_config_service.hpp`
- [ ] Create `src/services/cache_config_service.cpp`
- [ ] Move cache-related methods:
  - `getCacheConfig()`
  - `updateCacheConfig()`
  - `refreshCache()`
  - `performGarbageCollection()`
- [ ] Update ConfigService to delegate
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 6: Extract EndpointConfigService
- [ ] Create `src/services/endpoint_config_service.hpp`
- [ ] Create `src/services/endpoint_config_service.cpp`
- [ ] Move endpoint-related methods:
  - `listEndpoints()`
  - `createEndpoint()`
  - `getEndpointConfig()`
  - `updateEndpointConfig()`
  - `deleteEndpoint()`
  - `validateEndpointConfig()`
  - `reloadEndpointConfig()`
- [ ] Extract JSON conversion to `EndpointSerializer` class
- [ ] Update ConfigService to delegate
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 7: Extract ProjectConfigService
- [ ] Create `src/services/project_config_service.hpp`
- [ ] Create `src/services/project_config_service.cpp`
- [ ] Move `getProjectConfig()` and `updateProjectConfig()`
- [ ] Update ConfigService to delegate
- [ ] Run tests: `make test`
- [ ] Commit changes

## Phase 4: Cleanup & Optimization
### Step 8: Refactor ConfigService as Facade
- [ ] Remove all business logic from ConfigService
- [ ] Keep only route registration
- [ ] Initialize all sub-services in constructor
- [ ] Ensure clean delegation pattern
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 9: Add Helper Classes
- [ ] Create `ResponseBuilder` for consistent response formatting
- [ ] Create `RequestParser` for parameter extraction
- [ ] Create `PathResolver` utility (extract `resolveTemplatePath`)
- [ ] Update services to use helpers
- [ ] Run tests: `make test`
- [ ] Commit changes

### Step 10: Improve Error Handling
- [ ] Create custom exception classes
- [ ] Add consistent error response format
- [ ] Add request validation layer
- [ ] Run tests: `make test`
- [ ] Commit changes

## Phase 5: Testing & Documentation
### Step 11: Enhance Unit Tests
- [ ] Add unit tests for each new service
- [ ] Test services independently of HTTP layer
- [ ] Add mock ConfigManager for isolated testing
- [ ] Achieve >80% code coverage
- [ ] Run tests: `make test`

### Step 12: Integration Testing
- [ ] Run full integration test suite
- [ ] Test all API endpoints manually
- [ ] Verify no regression in functionality
- [ ] Performance testing

### Step 13: Documentation
- [ ] Update architecture documentation
- [ ] Add service class diagrams
- [ ] Document service responsibilities
- [ ] Update README with new structure

## Benefits After Refactoring
✅ **Single Responsibility**: Each service has one clear purpose
✅ **Testability**: Services can be unit tested independently
✅ **Maintainability**: Easier to locate and modify code
✅ **Extensibility**: Easy to add new features
✅ **Code Reuse**: Services can be reused by other components
✅ **Readability**: Smaller, focused classes are easier to understand

## Risk Mitigation
- Work incrementally (one service at a time)
- Run `make test` after each step
- Keep git commits small and focused
- Test manually after each phase
- Easy to rollback if issues arise


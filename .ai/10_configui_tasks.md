# flAPI Web UI Implementation Checklist

## Phase 0: Backend Completion (Critical)

### 1. Complete ConfigService Implementation
- [x] Implement missing endpoint handlers in `config_service.cpp`:
  - [x] All endpoint handlers implemented

### 2. Enhance Backend Testing
- [x] Add test cases for all endpoint handlers
- [x] Add test cases for error conditions and edge cases
- [x] Add test cases for cache management
- [x] Add test cases for schema management
- [x] Add test cases to verify that the files on disk are kept in sync

### 3. Backend Documentation
- [x] Document the API response formats for each endpoint
- [x] Document error codes and their meanings
- [x] Document the template expansion syntax and rules
- [x] Document the cache configuration options

## Phase 1: Project Setup and Basic Structure

### 1. Development Environment Setup
- [x] Install Node.js and npm
- [x] Set up SvelteKit project in `./web` directory
- [x] Install and configure shadcn-svelte
- [x] Set up TypeScript configuration

### 2. Build Process Integration
- [x] Set up basic build configuration
  - [x] Create svelte.config.js
  - [x] Create vite.config.ts
  - [x] Set up PostCSS and Tailwind
  - [x] Configure static build such that it produces a single output in `./web/dist/index.html`
- [x] Review and understand current `web/CMakeLists.txt` and `web/generate_embedded_ui.cmake`
  - [x] Create CMakeLists.txt for web directory
  - [x] Create generate_embedded_ui.cmake script which embeds the single file output into the binary
  - [x] Fix dependency issues
- [x] Ensure build process works on all platforms
  - [x] Test build on Linux
    - [x] Fix npm dependencies
    - [x] Fix CMake build scripts
    - [x] Test full build process
  - [o] Test build on Windows (later)
  - [o] Test build on MacOS (later)
- [x] Ensure that the frontend application is served by the backend
  - [x] Add embedded file serving in config_service.cpp
  - [x] Add content-type mapping for static files
  - [x] Test serving the web UI
- [x] Test whether a basic application works
  - [x] Test navigation between routes
  - [x] Test basic layout functionality
  - [x] Test embedded file serving
  - [x] Fix empty page issue
  - [x] Fix TypeScript/linter errors

### 3. Basic Application Structure
- [x] Create basic layout components
  - [x] Create `src/routes/+layout.svelte` for main layout
  - [x] Implement primary left sidebar
  - [x] Implement secondary right editor area
- [x] Set up routing structure
  - [x] `/config` - General configuration
  - [x] `/endpoints` - Endpoint management
  - [x] `/connections` - Connection management
- [x] Create TypeScript interfaces based on OpenAPI spec
  - [x] `ProjectConfig` interface
  - [x] `EndpointConfig` interface
  - [x] `CacheConfig` interface
  - [x] `ConnectionConfig` interface
  - [x] `AuthConfig` interface
  - [x] `RequestParameter` interface
  - [x] `RateLimitConfig` interface
  - [x] Add validation functions for all interfaces
  - [x] Add comprehensive test coverage

### 4. UI Component Implementation
- [ ] Set up shadcn-svelte components
  - [x] Install shadcn-svelte CLI
  - [x] Initialize shadcn-svelte
  - [x] Add button component
  - [x] Add sidebar component
  - [x] Set up theme configuration
    - [x] Create theme types and config
    - [x] Add theme store
    - [x] Add system theme detection

### 4. UI Component Implementation
- [ ] Set up shadcn-svelte components
  - [x] Install shadcn-svelte CLI
  - [x] Initialize shadcn-svelte
  - [x] Add button component
  - [x] Add sidebar component
  - [x] Set up theme configuration
    - [x] Create theme types and config
    - [x] Add theme store
    - [x] Add system theme detection
- [x] Update layout components to use shadcn components
  - [x] Create navigation menu component
  - [x] Update App.svelte with shadcn components
  - [x] Update route pages with shadcn typography

## Phase 2: API Integration

### 1. API Client Implementation
- [x] Create API client service (`src/lib/api.ts`)
  - [x] Project configuration endpoints
  - [x] Endpoint management endpoints
  - [x] Connection management endpoints
  - [x] Schema information endpoints
- [x] Create a test suite for the API client
- [x] Implement error handling
- [x] Add request/response interceptors
  - [x] Add request interceptor support
  - [x] Add response interceptor support
  - [x] Add interceptor removal functionality
  - [x] Add comprehensive tests
- [x] Add authentication handling
  - [x] Add basic auth support
  - [x] Add bearer token support
  - [x] Add AWS Secrets Manager configuration
  - [x] Add auth configuration endpoints
  - [x] Add comprehensive tests
- [x] Add type definitions for expanded configuration
  - [x] Add server configuration types (server_name, http_port, cache_schema)
  - [x] Add DuckDB settings type with flexible additionalProperties
  - [x] Add AWS Secrets Manager authentication types
  - [x] Add connection logging configuration types
  - [x] Add comprehensive tests for all configurations

### 2. State Management
- [x] Set up stores for global state
  - [x] Create store factory with loading/error handling
  - [x] Add store types and interfaces
  - [x] Project configuration store
  - [x] Add comprehensive tests
- [x] Endpoints store
  - [x] Basic CRUD operations
  - [x] Template management
  - [x] Add comprehensive tests
- [x] Connections store
  - [x] Basic CRUD operations
  - [x] Add comprehensive tests
- [x] Schema store
  - [x] Basic schema loading
  - [x] Add comprehensive tests
- [x] Implement loading states
  - [x] Create global loading state store
  - [x] Add request tracking
  - [x] Add derived loading state
- [x] Add error handling states
  - [x] Create global error state store
  - [x] Add error handling methods
  - [x] Add derived error state
  - [x] Add comprehensive tests
- [x] Add stores for advanced configuration
  - [x] Server configuration store
    - [x] Basic CRUD operations
    - [x] Add comprehensive tests
  - [x] DuckDB settings store
    - [x] Basic CRUD operations
    - [x] Add comprehensive tests
  - [x] AWS Secrets Manager store
    - [x] Test connection functionality
    - [x] Configuration management
    - [x] Add comprehensive tests
- [x] Add comprehensive store tests
  - [x] Add store interaction tests
  - [x] Test concurrent loading states
  - [x] Test error propagation
  - [x] Test store independence
  - [x] Add test coverage for edge cases

## Phase 3: Core Components Implementation

### 1. SQL Editor Component
- [x] Set up CodeMirror integration
  ```bash
  npm install @codemirror/state @codemirror/view @codemirror/commands @codemirror/language @codemirror/lang-sql @codemirror/theme-one-dark
  ```
- [x] Create reusable SQL editor component
  - [x] Basic editor functionality
  - [x] Syntax highlighting
  - [x] Proper TypeScript types
  - [x] Template variable support
    - [x] Variable insertion buttons
    - [x] Variable tooltips
    - [x] Mustache template syntax
  - [x] Output display area
    - [x] Collapsible output panel
    - [x] Pre-formatted output text
    - [x] Proper styling
  - [x] Schema browser integration
    - [x] Schema tree view
    - [x] Table and column display
    - [x] Table insertion
    - [x] Schema store integration
  - [x] SQL execution preview
    - [x] Parameter input fields
    - [x] Preview button
    - [x] Loading state
    - [x] Error handling
    - [x] Result display

### 2. Endpoint Editor Components
- [~] Create base endpoint editor layout
  - [x] Create EndpointEditor.svelte component
  - [x] Implement left sidebar for sections
  - [x] Set up main content area
  - [~] Add tests for layout structure
    - [x] Basic rendering tests
    - [x] Section switching tests
    - [ ] Fix button component mocking in tests
- [~] Implement general endpoint configuration section
  - [x] Create GeneralEndpointConfig.svelte
  - [x] Add fields from OpenAPI spec
    - [x] Endpoint path
    - [x] HTTP method
    - [x] Version
    - [x] Description
  - [x] Add validation
  - [~] Add tests
    - [x] Test component rendering
    - [x] Test value updates
    - [ ] Fix input component mocking in tests
- [~] Implement parameters section
  - [x] Create ParametersConfig.svelte
  - [x] Support parameter types
    - [x] String
    - [x] Number
    - [x] Boolean
  - [x] Support parameter locations
    - [x] Query parameters
    - [x] Path parameters
    - [x] Header parameters
  - [x] Add parameter fields
    - [x] Name
    - [x] Type
    - [x] Location
    - [x] Required flag
    - [x] Description
  - [x] Add parameter management
    - [x] Add new parameter
    - [x] Remove parameter
    - [x] Update parameter
  - [x] Add parameter validation
    - [x] Name format validation
    - [x] Required path parameters
    - [x] Unique parameter names
  - [~] Add tests
    - [x] Test component rendering
    - [x] Test parameter CRUD operations
    - [~] Test validation
      - [x] Test name format validation
      - [x] Test path parameter validation
      - [ ] Test duplicate name validation
- [ ] Implement query configuration
  - [ ] Create QueryConfig.svelte
  - [ ] Integrate SQL editor component
  - [ ] Add template variable support
  - [ ] Add tests
- [ ] Implement cache configuration
  - [ ] Create CacheConfig.svelte
  - [ ] Add cache settings fields
  - [ ] Add validation
  - [ ] Add tests

## Phase 4: Frontend Backend Integration
- [x] Check in general that the frontend can reach the backend
  - [x] Check if backend is reachable in embedded mode (served by flAPI binary)
  - [x] Check if backend is reachable in dev mode (served by `npm run dev`)
- [x] Add frontend backend integration
  - [x] Add frontend backend integration for project configuration
  - [x] Add frontend backend integration for connection configuration
    - [x] Ensure that existing connections are shown in the [navigation tree](../web/src/lib/components/navigation)
    - [x] Ensure that editing an existing connection works
    - [ ] Check that adding a new connection works
    - [ ] Ensure that new connections are added to the navigation tree
  - [ ] Add frontend backend integration for endpoint configuration
    - [ ] Ensure that existing endpoints are shown in the [navigation tree](../web/src/lib/components/navigation)
    - [ ] Ensure that editing an existing endpoint works
    - [ ] Ensure that adding a new endpoint works
    - [ ] Ensure that new endpoints are added to the navigation tree
    - [ ] Add frontend backend integration for query configuration
      - [ ] Add frontend backend integration for cache configuration
      - [ ] Add frontend backend integration for authentication configuration

## Phase 5: Advanced Features

### 1. Tree View Implementation
- [ ] Create tree view component for sidebar
  - [ ] Project structure representation
  - [ ] Endpoints listing
  - [ ] Connections listing
- [ ] Add context menus
- [ ] Implement drag-and-drop functionality

### 2. Testing Interface
- [ ] Create endpoint testing UI
  - [ ] Parameter input form
  - [ ] Request builder
  - [ ] Response viewer
- [ ] Implement connection testing UI
- [ ] Add cache testing functionality

### 3. Cache Management
- [ ] Implement cache configuration UI
- [ ] Add cache refresh controls
- [ ] Create cache status viewer

## Phase 6: Polish and Integration

### 1. UI/UX Improvements
- [ ] Add loading states
- [ ] Implement error handling UI
- [ ] Add success notifications
- [ ] Improve responsive design

### 2. Testing and Documentation
- [ ] Write component tests
- [ ] Add end-to-end tests
- [ ] Create user documentation
- [ ] Add inline help/tooltips

### 3. Build and Deployment
- [ ] Optimize build configuration
- [ ] Test embedded UI in flAPI binary
- [ ] Verify cross-platform functionality
- [ ] Add production optimizations

- [ ] Create authentication configuration component
  - [ ] Basic auth configuration
  - [ ] Bearer token configuration
  - [ ] AWS Secrets Manager configuration
    - [ ] Secret name input with validation
    - [ ] Region selector
    - [ ] Credentials input (secret_id, secret_key)
    - [ ] Secret table name configuration
    - [ ] Init SQL editor with template support
- [ ] Add authentication testing interface
  - [ ] Test basic auth credentials
  - [ ] Test AWS Secrets Manager connection
  - [ ] Preview generated init SQL

## Notes
- Start with basic functionality and iterate
- Follow shadcn-svelte component patterns
- Maintain TypeScript type safety throughout
- Regularly test on all supported platforms
- Keep bundle size in mind for embedded distribution

## Resources
- [shadcn-svelte Documentation](https://next.shadcn-svelte.com/)
- [SvelteKit Documentation](https://kit.svelte.dev/)
- [CodeMirror Documentation](https://codemirror.net/)
- [OpenAPI Specification](./10_openapi.yaml) 
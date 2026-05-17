## User Story: Web-Based User Interface for Endpoint and Connection Management in flAPI

### **Title**: Develop a Web UI for Adding, Editing, and Testing Endpoints and Connections in flAPI

### **Background**:  
Currently, flAPI relies on YAML files and SQL templates for configuring endpoints and connections, which requires manual editing and technical knowledge. A web-based user interface (UI) will simplify endpoint and connection management, making it accessible to non-technical users and speeding up development. The UI is primarly for data engineers and data scientists to come up with new endpoints and connections, and not have to learn the flapi YAML syntax.

### **Goal**:
Build a web-based user interface that allows users to add, modify, and test API endpoints and connections without editing YAML files manually. The new configuration shold always be in sync with files on the filesystem. The backend and UI should provide a way to test and validate the configuration before saving. 

---
The main configuration is defined in `src/include/config_manager.hpp` and `src/config_manager.cpp`. Example configuration files are in `./examples` directory.
---

## **Feature Requirements:**

### **Backend Requirements:**
- The backend should be implemented (and is already partially implemented) in `src/include/config_service.hpp` and `src/config_service.cpp`.
- The API of the config service is sketched in `10_openapi.yaml`.
- Reuse the functionality of existing backend functionality:
    - `src/include/database_manager.hpp` and `src/database_manager.cpp`
    - `src/include/cache_manager.hpp` and `src/cache_manager.cpp`
    - `src/include/sql_template_processor.hpp` and `src/sql_template_processor.cpp`
    - `src/include/config_manager.hpp` and `src/config_manager.cpp`
    - `src/include/database_manager.hpp` and `src/database_manager.cpp`
- The backend should provide a way to test and validate the configuration before saving.
- The backend should provide a way to save the configuration to the filesystem.
- The backend should provide a way to load the configuration from the filesystem.
- The backend should provide to test connection init SQL statements.
- The backend should provide to expand the SQL template and test cache SQL definitions. 
- The backend should provide to expand the SQL template and test endpoint SQL templates.
- For all backend functionality there should be a test case in `test/cpp/config_service_test.cpp`. The tests should be implemented similar to the existing tests and use the catch2 framework.
- Later the idea is to version the configurations via git.
- The backend should provide an enpoint which servers the frontent UI. All static parts should be embedded in the static flapi binary. This has to work on all supported platforms (Linux, Windows, MacOS).

### **Frontend Requirements:**

- The UI should be implemented in `./web` directory.
- The current implementation could be completetly changed and rewritten.
- The application should be implemented in typescript and sveltekit. 
- The UI library should use https://next.shadcn-svelte.com/ as component library.
- Extablish a cmake based build process which makes it easy to embed the UI into the flapi binary.

- **Layout Overview:**
  - The UI should resemble a lightweight IDE, similar to VSCode.
  - The main layout consists of:
    1. **Primary Left Sidebar Panel** - Displays a tree view of the base `flapi.yaml` configuration, all connections and all endpoints. (Use https://next.shadcn-svelte.com/docs/components/sidebar). A possibility to add new connections and endpoints should be provided in this sidebar panel.
    2. **Secondary Right Editor Area** - Displays the configuration editors for the selected item.
        2.1 **General Configuration Editor** - A primary UI for editing the general `flapi.yaml` configuration file. The fields should be taken from the openapi specification of the backend service.
        2.2 **Connection Editor** - A primary UI for editing the connection configuration. The fields should be taken from the openapi specification of the backend service. The init SQL statement should reuse the general SQL editor component (defined below).
        2.3 **Endpoint Editor** - A primary UI for editing the endpoint configuration. The fields should be taken from the openapi specification of the backend service. The SQL template should reuse the general SQL editor component (defined below).
            - The Endpoint edtor should have a secondary sidebar on the left side having sections for:
                - General Endpoint Configuration (like endpoint path, method, version, etc.)
                - Input and Path Parameters with validation
                - Query Configuration
                - Cache Configuration
                - Authentication Configuration
                - Rate Limiting Configuration
                - Endpoint Testing Interface
    

- **General Configuration Editor:**
  - A primary UI for editing the general `flapi.yaml` configuration file.
  - Sections for each and every configuration option in `flapi.yaml`.

- **Connection Editor (Sub-Editor):**
  - Integrated as a sub-editor within the general configuration editor.
  - Configures individual connections specified in `flapi.yaml`.
  - Provides functionality to test connections interactively.
  - Validates connection configurations and displays error messages or success feedback.
  - Allows editing initialization scripts and connection properties.

- **Endpoint Editor:**
  - Each endpoint is managed via a main `.yaml` file.
  - New endpoints are created in dedicated directories for better organization.
  - Multi-pane design resembling a developer IDE (e.g., VSCode).
  - Top pane for configuring different elements of an endpoint:
    - **Left Panel:** Selection of endpoint configuration.
    - **Center Panel:** Cache settings, including cache configuration and SQL definitions.
    - **Right Panel:** Input and path parameters, validators, and metadata settings.
  - SQL template editor with syntax highlighting, validation, and testing.
  - Real-time preview and execution of SQL queries.
  - Displays results and error messages interactively.
  - Allows testing endpoint configurations by sending HTTP requests directly from the UI.
  - Displays response data, HTTP status codes, headers, and execution times.
  - Supports adding and configuring caching, authentication, and rate limits.

- **SQL Editor Component:**
  - A reusable component for editing SQL templates. The Templates are essnetially [Mustache templates](https://mustache.github.io/mustache.5.html) which are rendered with the [Crow mustanche]. For the user of the editor component it is essential to understand this template expansion
  - Use the https://codemirror.net/ library for the main editor.
  - The editor component should have the possibility to show output below the editor.
    - This output could be the result of the SQL template expansion.
    - This output could be the result of the SQL query execution.
  - The editor should have the possibility to show available variables and parameters. When clicking on a variable or parameter the editor should insert the variable or parameter into the editor at the current cursor position.
  - The editor should have the possibility to the current schemas of the underlying Duckdb database.
---


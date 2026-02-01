# Makefile for flAPI project

# Phony targets
.PHONY: all debug release clean run-debug run-release run-integration-tests docker-build web cli-build cli-test vscode-build vscode-dev integration-test integration-tests integration-test-rest integration-test-mcp integration-test-ducklake integration-test-examples integration-test-setup integration-test-ci test-all help

# Check if Ninja is available
NINJA := $(shell which ninja)
CMAKE_GENERATOR := $(if $(NINJA),-G Ninja,)

# Compiler and flags
ifeq ($(OS),Windows_NT)
    CXX := cl.exe
    CMAKE := cmake
    # Use forward slashes and MINGW-compatible commands
    RM := rm -f
    MKDIR := mkdir -p
    RMDIR := rm -rf
    CMAKE_GENERATOR := -G "Visual Studio 17 2022" -A x64
else
    CXX := g++
    CMAKE := cmake
    RM := rm -f
    MKDIR := mkdir -p
    RMDIR := rm -rf
endif

# Build directories
BUILD_DIR := build
DEBUG_DIR := $(BUILD_DIR)/debug
RELEASE_DIR := $(BUILD_DIR)/release

# Docker image name
DOCKER_FILE := Dockerfile
DOCKER_IMAGE_NAME := ghcr.io/datazoode/flapi

# Default target
all: debug release

# Help target
help:
	@echo "Available targets:"
	@echo "  Build targets:"
	@echo "    all                    - Build both debug and release versions"
	@echo "    debug                  - Build debug version"
	@echo "    release                - Build release version"
	@echo "    clean                  - Clean all build artifacts"
	@echo ""
	@echo "  Run targets:"
	@echo "    run-debug              - Run debug version with example config"
	@echo "    run-release            - Run release version with example config"
	@echo ""
	@echo "  Test targets:"
	@echo "    test                   - Run unit tests only"
	@echo "    test-all               - Run all tests (unit + integration)"
	@echo "    integration-test       - Run all integration tests"
	@echo "    integration-test-rest  - Run REST API integration tests (Tavern)"
	@echo "    integration-test-mcp   - Run MCP integration tests"
	@echo "    integration-test-ducklake - Run DuckLake integration tests"
	@echo "    integration-test-examples - Run examples integration tests"
	@echo "    integration-test-ci    - Run integration tests with server management (CI/CD)"
	@echo "    integration-test-setup - Setup Python environment for integration tests"
	@echo ""
	@echo "  Web targets:"
	@echo "    web                    - Build web application"
	@echo "    web-clean              - Clean web build artifacts"
	@echo ""
	@echo "  CLI targets:"
	@echo "    cli-build              - Build Node.js CLI"
	@echo "    cli-test               - Run Node.js CLI tests"
	@echo ""
	@echo "  VSCode targets:"
	@echo "    vscode-build           - Build VSCode extension"
	@echo "    vscode-dev             - Start VSCode extension development"
	@echo ""
	@echo "  Docker targets:"
	@echo "    docker                 - Build Docker image"
	@echo ""
	@echo "  Legacy targets:"
	@echo "    run-integration-tests  - Legacy integration test target"

# Debug build for Windows (Visual Studio compatible)
ifeq ($(OS),Windows_NT)
debug:
	@echo "Building debug version for Windows..."
	@$(MKDIR) $(DEBUG_DIR)
	@cd $(DEBUG_DIR) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Debug \
		$(CMAKE_GENERATOR) ../..
	@$(CMAKE) --build $(DEBUG_DIR) --config Debug
else ifeq ($(shell uname),Darwin)
    # Detect host architecture
    HOST_ARCH := $(shell uname -m)
    
    debug:
	@echo "Building debug version for $(HOST_ARCH)..."
	@mkdir -p $(DEBUG_DIR)
	@cd $(DEBUG_DIR) && \
	VCPKG_DEFAULT_TRIPLET=$(if $(filter arm64,$(HOST_ARCH)),arm64-osx,x64-osx) \
	$(CMAKE) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_OSX_ARCHITECTURES=$(HOST_ARCH) \
		-DVCPKG_TARGET_TRIPLET=$(if $(filter arm64,$(HOST_ARCH)),arm64-osx,x64-osx) \
		$(CMAKE_GENERATOR) ../..
	@$(CMAKE) --build $(DEBUG_DIR) --config Debug
else
    # Original debug target for non-macOS platforms
    debug: $(DEBUG_DIR)/build.ninja
	@echo "Building debug version..."
	@$(CMAKE) --build $(DEBUG_DIR) --config Debug
endif

$(DEBUG_DIR)/build.ninja:
	@mkdir -p $(DEBUG_DIR)
	@cd $(DEBUG_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Debug $(CMAKE_GENERATOR) $(CMAKE_EXTRA_FLAGS) ../..

# macOS specific variables
ifeq ($(shell uname),Darwin)
    BUILD_UNIVERSAL := build/universal
    
    release-universal: release-x86_64 release-arm64
	@echo "Creating universal binary..."
	@mkdir -p $(BUILD_UNIVERSAL)
	@lipo -create \
		$(RELEASE_DIR)-x86_64/flapi \
		$(RELEASE_DIR)-arm64/flapi \
		-output $(BUILD_UNIVERSAL)/flapi
	@echo "Universal binary created at $(BUILD_UNIVERSAL)/flapi"

    release-x86_64:
	@echo "Building release version for x86_64..."
	@mkdir -p $(RELEASE_DIR)-x86_64
	@cd $(RELEASE_DIR)-x86_64 && \
	VCPKG_DEFAULT_TRIPLET=x64-osx \
	$(CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=x86_64 \
		-DVCPKG_TARGET_TRIPLET=x64-osx \
		-DBUILD_TESTING=ON \
		$(CMAKE_GENERATOR) ../..
	@$(CMAKE) --build $(RELEASE_DIR)-x86_64 --config Release

    release-arm64:
	@echo "Building release version for arm64..."
	@mkdir -p $(RELEASE_DIR)-arm64
	@cd $(RELEASE_DIR)-arm64 && \
	VCPKG_DEFAULT_TRIPLET=arm64-osx \
	$(CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=arm64 \
		-DVCPKG_TARGET_TRIPLET=arm64-osx \
		-DBUILD_TESTING=ON \
		$(CMAKE_GENERATOR) ../..
	@$(CMAKE) --build $(RELEASE_DIR)-arm64 --config Release

    # Override the default release target on macOS
    release: release-universal
else
    # Linux/Windows release builds with cross-compilation support
    release:
	@echo "Building release version $(if $(CROSS_COMPILE),for $(CROSS_COMPILE),native)..."
	@mkdir -p $(RELEASE_DIR)
	# @$(MAKE) web
	@cd $(RELEASE_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
		$(CMAKE_GENERATOR) $(CMAKE_EXTRA_FLAGS) ../..
	@$(CMAKE) --build $(RELEASE_DIR) --config Release
endif

# Clean build directories - Windows (MINGW compatible)
ifeq ($(OS),Windows_NT)
clean: web-clean
	@echo "Cleaning build directories..."
	@if [ -d "$(BUILD_DIR)" ]; then $(RMDIR) $(BUILD_DIR); fi
	@cd duckdb && $(MAKE) clean
else
clean: web-clean
	@echo "Cleaning build directories..."
	@rm -rf $(BUILD_DIR)
	@cd duckdb && $(MAKE) clean
endif

# Run debug version
run-debug: debug
	@echo "Running debug version..."
	@$(DEBUG_DIR)/flapi --config examples/flapi.yaml --log-level debug

# Run debug tests
run-debug-tests: debug
	@echo "Running debug tests..."
	@$(DEBUG_DIR)/test/cpp/flapi_tests

# Run release version
run-release: release
	@echo "Running release version..."
	@$(RELEASE_DIR)/flapi --config examples/flapi.yaml --log-level info

# Legacy integration tests target (kept for compatibility)
run-integration-tests: debug
	@echo "Running integration tests..."
	$(CMAKE) --build $(DEBUG_DIR) --target integration_tests

# Integration test setup - install Python dependencies
integration-test-setup:
	@echo "Setting up integration test environment..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		echo "Using uv for dependency management..."; \
		uv sync; \
	elif command -v pip >/dev/null 2>&1; then \
		echo "Using pip for dependency management..."; \
		pip install tavern pytest pytest-asyncio pytest-timeout requests pandas pyarrow mcp anthropic python-dotenv; \
	else \
		echo "Error: Neither uv nor pip found. Please install Python package manager."; \
		exit 1; \
	fi
	@echo "Integration test setup completed"

# Run all integration tests in parallel
integration-test: release integration-test-setup
	@echo "Running all integration tests in parallel..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest \
			--ignore=test_load_testing.py \
			--timeout=120 \
			-n auto \
			-v \
			--tb=short; \
	else \
		source .venv/bin/activate && pytest \
			--ignore=test_load_testing.py \
			--timeout=120 \
			-n auto \
			-v \
			--tb=short; \
	fi
	@echo "All integration tests completed"

# Comprehensive integration sweep (includes skipped/slow suites)
integration-tests: release integration-test-setup
	@echo "Running comprehensive integration suite (with per-file timeouts)..."
	@cd test/integration && UV_CACHE_DIR=/tmp/uv-cache \
		python ../../tools/run_full_integration_suite.py
	@echo "Comprehensive integration sweep completed. See diagnosis/integration_test_results.json for details."

# Run REST API integration tests (Tavern-based)
integration-test-rest: release integration-test-setup
	@echo "Running REST API integration tests..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest test_customers.tavern.yaml test_customers_cached.tavern.yaml test_data_types.tavern.yaml test_request_validation.tavern.yaml -v; \
	else \
		pytest test_customers.tavern.yaml test_customers_cached.tavern.yaml test_data_types.tavern.yaml test_request_validation.tavern.yaml -v; \
	fi

# Run DuckLake integration tests
integration-test-ducklake: release integration-test-setup
	@echo "Running DuckLake integration tests..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest test_ducklake_scheduler.py -v; \
	else \
		pytest test_ducklake_scheduler.py -v; \
	fi

# Run MCP integration tests
integration-test-mcp: release integration-test-setup
	@echo "Running MCP integration tests..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest test_mcp_integration.py -v; \
	else \
		pytest test_mcp_integration.py -v; \
	fi

# Run examples integration tests
integration-test-examples: release integration-test-setup
	@echo "Running examples integration tests..."
	@cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest test_examples_northwind.py test_examples_customers.py test_examples_mcp.py -v; \
	else \
		pytest test_examples_northwind.py test_examples_customers.py test_examples_mcp.py -v; \
	fi

# Run integration tests with server startup (for CI/CD)
integration-test-ci: release integration-test-setup
	@echo "Running integration tests with server management..."
	@echo "Starting flapi server in background..."
	@if [ "$(shell uname)" = "Darwin" ]; then \
		FLAPI_BIN=$(BUILD_DIR)/universal/flapi; \
	else \
		FLAPI_BIN=$(RELEASE_DIR)/flapi; \
	fi; \
	$$FLAPI_BIN --config examples/flapi.yaml --log-level info --config-service --config-service-token test-token & \
	SERVER_PID=$$!; \
	echo "Server started with PID: $$SERVER_PID"; \
	sleep 5; \
	echo "Running integration tests..."; \
	cd test/integration && \
	if command -v uv >/dev/null 2>&1; then \
		uv run pytest -v --timeout=300; \
	else \
		pytest -v --timeout=300; \
	fi; \
	TEST_RESULT=$$?; \
	echo "Stopping server..."; \
	kill $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	echo "Integration tests completed with exit code: $$TEST_RESULT"; \
	exit $$TEST_RESULT

# Build Docker image
docker: release
	@echo "Building Docker image..."
	@mkdir -p binaries/amd64 binaries/arm64
	@if [ "$(shell uname)" = "Darwin" ]; then \
		if [ "$(shell uname -m)" = "x86_64" ]; then \
			cp $(RELEASE_DIR)-x86_64/flapi binaries/amd64/flapi; \
		else \
			cp $(RELEASE_DIR)-arm64/flapi binaries/arm64/flapi; \
		fi; \
	else \
		cp $(RELEASE_DIR)/flapi binaries/$(shell uname -m | sed 's/x86_64/amd64/' | sed 's/aarch64/arm64/')/flapi; \
	fi
	@if [ ! -f binaries/amd64/flapi ] && [ ! -f binaries/arm64/flapi ]; then \
		echo "Error: Could not find flapi binary"; \
		exit 1; \
	fi
	@ARCH=$$(if [ "$$(uname -m)" = "x86_64" ]; then echo "amd64"; elif [ "$$(uname -m)" = "aarch64" ]; then echo "arm64"; else echo "unknown"; fi); \
	if [ "$$ARCH" = "unknown" ]; then \
		echo "Unsupported architecture: $$(uname -m)"; \
		exit 1; \
	fi; \
	DOCKER_BUILDKIT=1 docker build \
		--platform=linux/$$ARCH \
		--build-context build=binaries \
		--progress=plain \
		-t $(DOCKER_IMAGE_NAME):latest \
		-f $(DOCKER_FILE) .

# Add a test target (unit tests only)
test: release
	@echo "Running unit tests..."
	@if [ "$(shell uname)" = "Darwin" ]; then \
		cd $(RELEASE_DIR)-$(shell uname -m | sed 's/x86_64/x86_64/' | sed 's/arm64/arm64/') && \
		ctest --output-on-failure; \
	else \
		cd $(RELEASE_DIR) && \
		ctest --output-on-failure; \
	fi

# Run all tests (unit + integration)
test-all: test integration-test
	@echo "All tests completed successfully"

web-clean:
	@echo "Cleaning web build artifacts..."
	@rm -rf web/package-lock.json
	@rm -rf web/.svelte-kit
	@rm -rf web/dist/
	@rm -rf web/node_modules/
	@rm -rf src/include/embedded/
	@echo "Web clean completed"

web:
	@echo "Building web application..."
	@$(MAKE) web-clean
	@cd web && npm install
	@cd web && npm run build
	@if [ ! -f "web/dist/index.html" ]; then \
		echo "Error: Build failed - index.html not found"; \
		exit 1; \
	fi
	@echo "Web build completed successfully"

cli-build:
	@echo "Building Node.js CLI..."
	@npm --prefix cli install --no-fund
	@npm --prefix cli run build

cli-test:
	@echo "Running Node.js CLI tests..."
	@npm --prefix cli install --no-fund
	@npm --prefix cli run test

vscode-build:
	@echo "Building VSCode extension..."
	@npm --prefix cli/shared install --no-fund
	@npm --prefix cli/shared run build
	@npm --prefix cli/vscode-extension install --no-fund
	@npm --prefix cli/vscode-extension run build

vscode-dev:
	@echo "Starting VSCode extension development build..."
	@npm --prefix cli/shared install --no-fund
	@npm --prefix cli/shared run build -- --watch
	@npm --prefix cli/vscode-extension install --no-fund
	@npm --prefix cli/vscode-extension run dev

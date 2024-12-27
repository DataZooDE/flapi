# Makefile for flAPI project

# Phony targets
.PHONY: all debug release clean run-debug run-release run-integration-tests docker-build web

# Compiler and flags
CXX := g++
CMAKE := cmake

# Check if Ninja is available
NINJA := $(shell which ninja)
CMAKE_GENERATOR := $(if $(NINJA),-G Ninja,)

# Build directories
BUILD_DIR := build
DEBUG_DIR := $(BUILD_DIR)/debug
RELEASE_DIR := $(BUILD_DIR)/release

# Docker image name
DOCKER_FILE := Dockerfile
DOCKER_IMAGE_NAME := ghcr.io/datazoode/flapi

# Default target
all: debug release

# Debug build
ifeq ($(shell uname),Darwin)
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
		-DBUILD_TESTING=OFF \
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
		-DBUILD_TESTING=OFF \
		$(CMAKE_GENERATOR) ../..
	@$(CMAKE) --build $(RELEASE_DIR)-arm64 --config Release

    # Override the default release target on macOS
    release: release-universal
else
    # Linux release builds with cross-compilation support
    release: $(RELEASE_DIR)/build.ninja
	@echo "Building release version $(if $(CROSS_COMPILE),for $(CROSS_COMPILE),native)..."
	@$(CMAKE) --build $(RELEASE_DIR) --config Release
endif

$(RELEASE_DIR)/build.ninja:
	@mkdir -p $(RELEASE_DIR)
	@cd $(RELEASE_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
		$(CMAKE_GENERATOR) $(CMAKE_EXTRA_FLAGS) ../..

# Clean build directories
clean:
	@echo "Cleaning build directories..."
	cd duckdb && make clean
	@rm -rf $(BUILD_DIR)

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

# Run release tests
run-release-tests: release
	@echo "Running release tests..."
	@$(RELEASE_DIR)/test/cpp/flapi_tests

# Run integration tests
run-integration-tests: debug
	@echo "Running integration tests..."
	@$(CMAKE) --build $(DEBUG_DIR) --target integration_tests

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
		--build-context build=binaries/$$ARCH \
		-t $(DOCKER_IMAGE_NAME):latest \
		-f $(DOCKER_FILE) .

# Add a test target
test: release
	@echo "Running tests..."
	@cd $(RELEASE_DIR)-$(shell uname -m | sed 's/x86_64/x86_64/' | sed 's/arm64/arm64/') && \
	ctest --output-on-failure

web:
	cd web && npm install && npm run build

web-clean:
	rm -rf web/package-lock.json
	rm -rf web/.svelte-kit
	rm -rf web/build/
	rm -rf web/node_modules/

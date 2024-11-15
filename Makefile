# Makefile for flAPI project

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
DOCKER_FILE := docker/development/Dockerfile
DOCKER_IMAGE_NAME := ghcr.io/datazoode/flapi

# Default target
all: debug release

# Debug build
debug: $(DEBUG_DIR)/build.ninja
	@echo "Building debug version..."
	@$(CMAKE) --build $(DEBUG_DIR) --config Debug

$(DEBUG_DIR)/build.ninja:
	@mkdir -p $(DEBUG_DIR)
	@cd $(DEBUG_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Debug $(CMAKE_GENERATOR) ../..

# Release build
release: $(RELEASE_DIR)/build.ninja
	@echo "Building release version..."
	@$(CMAKE) --build $(RELEASE_DIR) --config Release
	@echo "Stripping debug symbols from release build..."
	@strip $(RELEASE_DIR)/flapi

$(RELEASE_DIR)/build.ninja:
	@mkdir -p $(RELEASE_DIR)
	@cd $(RELEASE_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Release $(CMAKE_GENERATOR) ../..

# Clean build directories
clean:
	@echo "Cleaning build directories..."
	@rm -rf $(BUILD_DIR)

# Run debug version
run-debug: debug
	@echo "Running debug version..."
	@$(DEBUG_DIR)/flapi --config examples/flapi.yaml --log-level debug

# Run release version
run-release: release
	@echo "Running release version..."
	@$(RELEASE_DIR)/flapi --config examples/flapi.yaml --log-level info

run-integration-tests: debug
	@echo "Running integration tests..."
	@$(CMAKE) --build $(DEBUG_DIR) --target integration_tests

# Build Docker image
docker: release
	@echo "Building Docker image..."
	docker build -t $(DOCKER_IMAGE_NAME):latest -f $(DOCKER_FILE) .

# Phony targets
.PHONY: all debug release clean run-debug run-release run-integration-tests docker-build

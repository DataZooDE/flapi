---
sidebar_position: 2
---

# Building from Source

## Prerequisites

- C++ compiler with C++17 support
- CMake 3.15 or higher
- Ninja build system (recommended)
- Git
- vcpkg package manager

On Ubuntu/Debian, you can install the basic requirements with:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build
```

## Build Steps

1. Clone the repository with submodules:
```bash
git clone --recurse-submodules https://github.com/datazoode/flapi.git
cd flapi
```

2. Initialize DuckDB submodule (we use version 1.1.2):
```bash
git submodule update --init --recursive
cd duckdb
git checkout v1.1.2
cd ..
```

3. Set up vcpkg:
```bash
# Bootstrap vcpkg if you haven't already
./vcpkg/bootstrap-vcpkg.sh

# Integrate vcpkg with your system
./vcpkg/vcpkg integrate install
```

4. Build the project:
```bash
# Create build directory
mkdir -p build/release
cd build/release

# Configure with CMake
cmake ../.. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=../../vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
ninja

# The binary will be available as 'flapi' in the current directory
```

## Verifying the Build

After building, you can verify the installation:

```bash
# Check if the binary runs
./flapi --version

# Optional: Move to system path for global access
sudo cp flapi /usr/local/bin/
```

## Common Build Issues

1. **Missing Dependencies**: If you encounter missing dependency errors, make sure vcpkg is properly initialized and integrated.

2. **CMake Configuration Fails**: Ensure you have the correct version of CMake and all required development tools installed.

3. **Build Memory Issues**: If the build process fails due to memory constraints, you can limit parallel jobs:
```bash
ninja -j2  # Limit to 2 parallel jobs
```

4. **vcpkg Cache**: To speed up future builds, vcpkg caches packages. The cache is typically located in `~/.cache/vcpkg`.

## Development Setup

For development, you might want to build with debug symbols:

```bash
mkdir -p build/debug
cd build/debug
cmake ../.. -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=../../vcpkg/scripts/buildsystems/vcpkg.cmake
ninja
```

The debug build will include additional information useful for development and debugging.
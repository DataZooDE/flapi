cmake_minimum_required(VERSION 3.15)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows-specific settings
if(WIN32)
    # Use dynamic runtime libraries for Windows builds
    add_definitions(-D_WIN32_WINNT=0x0601)  # Target Windows 7 or later

    # Set vcpkg triplet for Windows
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        if(MINGW)
            set(VCPKG_TARGET_TRIPLET "x64-mingw-dynamic" CACHE STRING "")
        else()
            set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
        endif()
    endif()
endif()

if(APPLE)
    # On macOS, automatically detect and use native architecture if not specified
    if(NOT CMAKE_OSX_ARCHITECTURES)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(CMAKE_OSX_ARCHITECTURES ${HOST_ARCH})
    endif()
    
    # Set vcpkg triplet based on architecture
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(VCPKG_TARGET_TRIPLET "arm64-osx")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-osx")
    endif()
    
    # Set paths based on triplet
    set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}")
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/share")
    
    message(STATUS "Building for architecture: ${CMAKE_OSX_ARCHITECTURES}")
    message(STATUS "Using vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")
endif()

if(UNIX AND NOT APPLE)
    message(STATUS "Cross compile env variable: $ENV{FLAPI_CROSS_COMPILE}")

    # Check for cross-compilation before project() is called
    if(DEFINED ENV{FLAPI_CROSS_COMPILE} AND "$ENV{FLAPI_CROSS_COMPILE}" STREQUAL "arm64")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_SYSTEM_PROCESSOR arm64)
        set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
        set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
        set(CMAKE_FIND_ROOT_PATH aarch64-linux-gnu)
        set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

        # Set vcpkg triplets for cross-compilation
        set(VCPKG_TARGET_TRIPLET "arm64-linux")
        set(VCPKG_HOST_TRIPLET "x64-linux")
        
        # Set up search paths for cross-compiled packages
        set(CMAKE_PREFIX_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}"
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/share"
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/lib/cmake"
        )
        set(CMAKE_FIND_ROOT_PATH
            ${CMAKE_FIND_ROOT_PATH}
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}"
        )
        
        # Set pkg-config path for cross-compilation
        set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/lib")
        
        message(STATUS "Cross-compiling for arm64")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-linux")
        message(STATUS "Building for native architecture: x86_64")
    endif()

    message(STATUS "Target triplet: ${VCPKG_TARGET_TRIPLET}")
    message(STATUS "Host triplet: ${VCPKG_HOST_TRIPLET}")
    message(STATUS "VCPKG root: $ENV{VCPKG_ROOT}")
    message(STATUS "Toolchain file: $ENV{VCPKG_TOOLCHAIN_PATH}")
endif()

# Set vcpkg toolchain file
if (DEFINED ENV{VCPKG_TOOLCHAIN_PATH})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_TOOLCHAIN_PATH}" CACHE STRING "Vcpkg toolchain file")
else()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

# Set project name and version
project(flAPI VERSION 0.3.0 LANGUAGES CXX)

# Basic settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
set(CROW_ENABLE_COMPRESSION ON)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Disable unused parameter warnings but keep other warnings
    set(COMMON_FLAGS "-Wall -Wextra -Wno-unused-parameter -Wno-redundant-move -Wno-maybe-uninitialized -Wno-cpp -Wno-type-limits")
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
        set(ENABLE_SANITIZER FALSE)
        set(ENABLE_UBSAN FALSE)
        
        if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Disable sanitizers on macOS/Clang
            set(SANITIZER_FLAGS "-fno-omit-frame-pointer")
            add_definitions(-DDUCKDB_NO_SANITIZER)
        endif()
        
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -O0 -g ${SANITIZER_FLAGS}")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -O2")
    endif()
endif()

# Linux-specific linker flags
if(UNIX AND NOT APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")

    # Check if mold linker is available
    find_program(MOLD_LINKER mold)

    if(MOLD_LINKER)
        message(STATUS "mold linker found: ${MOLD_LINKER}")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=mold")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=mold")
    else()
        message(STATUS "mold linker NOT found, using default linker.")
    endif()
endif()

option(ENABLE_SANITIZER "Enable address sanitizer." TRUE)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer." FALSE)
option(ENABLE_UBSAN "Enable undefined behavior sanitizer." TRUE)

# Find dependencies
find_package(OpenSSL REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 2f86d13
)
FetchContent_MakeAvailable(yaml-cpp)

# Add nanoarrow for Arrow IPC streaming support
set(NANOARROW_IPC ON CACHE BOOL "Build nanoarrow IPC extension")
set(NANOARROW_IPC_WITH_ZSTD ON CACHE BOOL "Build nanoarrow with ZSTD compression")
set(NANOARROW_IPC_WITH_LZ4 ON CACHE BOOL "Build nanoarrow with LZ4 compression")
set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "Disable nanoarrow tests")
FetchContent_Declare(
  nanoarrow
  GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
  GIT_TAG apache-arrow-nanoarrow-0.7.0
)
FetchContent_MakeAvailable(nanoarrow)

find_package(AWSSDK CONFIG COMPONENTS core secretsmanager REQUIRED)

# Find compression libraries for Arrow IPC compression
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

# Add DuckDB
set(DUCKDB_EXPLICIT_VERSION 1.4.4)
set(ENABLE_EXTENSION_AUTOLOADING true)
set(ENABLE_EXTENSION_AUTOINSTALL true)
if (WIN32)
    set(DUCKDB_EXPLICIT_PLATFORM "windows_amd64")
endif()

add_subdirectory(duckdb EXCLUDE_FROM_ALL)

# Include directories
find_path(JWT_CPP_INCLUDE_DIRS "jwt-cpp/base.h")
include_directories(
    src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include
    ${CROW_INCLUDE_DIRS}
    ${JWT_CPP_INCLUDE_DIRS}
    ${AWSSDK_INCLUDE_DIRS}
)

# Add web UI
# message(STATUS "Adding web UI subdirectory...")
# add_subdirectory(web)
# message(STATUS "Web UI subdirectory added")

# Create flapi-lib
add_library(flapi-lib STATIC
    src/api_server.cpp
    src/auth_middleware.cpp
    src/cache_manager.cpp
    src/config_loader.cpp
    src/endpoint_repository.cpp
    src/config_validator.cpp
    src/config_serializer.cpp
    src/config_manager.cpp
    src/config_service.cpp
    src/database_manager.cpp
    src/endpoint_config_parser.cpp
    src/extended_yaml_parser.cpp
    src/heartbeat_worker.cpp
    src/open_api_doc_generator.cpp
    src/path_utils.cpp
    src/query_executor.cpp
    src/request_handler.cpp
    src/request_validator.cpp
    src/rate_limit_middleware.cpp
    src/route_translator.cpp
    src/sql_template_processor.cpp
    src/mcp_server.cpp
    src/mcp_tool_handler.cpp
    src/mcp_route_handlers.cpp
    src/mcp_session_manager.cpp
    src/mcp_error_builder.cpp
    src/mcp_request_validator.cpp
    src/mcp_client_capabilities.cpp
    src/config_tool_adapter.cpp
    src/mcp_content_types.cpp
    src/mcp_auth_handler.cpp
    src/oidc_discovery_client.cpp
    src/oidc_jwks_manager.cpp
    src/oidc_auth_handler.cpp
    src/oidc_provider_presets.cpp
    src/http_client.cpp
    src/config_token_utils.cpp
    src/error.cpp
    src/type_converter.cpp
    src/vfs_adapter.cpp
    src/path_validator.cpp
    src/vfs_health_checker.cpp
    src/caching_file_provider.cpp
    src/credential_manager.cpp
)

# Ensure web_ui is built before flapi-lib
#message(STATUS "Setting up web UI dependencies...")
#add_dependencies(flapi-lib web_ui)

# Link against web_ui to get its include directories
#target_link_libraries(flapi-lib PUBLIC web_ui_lib)
#message(STATUS "Web UI dependencies configured")

# Link libraries to flapi-lib
target_link_libraries(flapi-lib PUBLIC
    duckdb_static
    Crow::Crow
    yaml-cpp::yaml-cpp
    jwt-cpp::jwt-cpp
    argparse::argparse
    OpenSSL::Crypto
    OpenSSL::SSL
    ${AWSSDK_LIBRARIES}
    fmt::fmt
    CURL::libcurl
    nanoarrow::nanoarrow
    nanoarrow::nanoarrow_ipc
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
    $<IF:$<TARGET_EXISTS:lz4::lz4>,lz4::lz4,lz4::lz4_shared>
)

# Add RPATH settings for macOS
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# Create main executable
add_executable(flapi src/main.cpp)
target_link_libraries(flapi PRIVATE flapi-lib)
set_target_properties(flapi PROPERTIES ENABLE_EXPORTS TRUE)

# Add Windows-specific libraries
if(WIN32)
    target_link_libraries(flapi PRIVATE Dbghelp.lib)
endif()

# Tests
include(CTest)
include(Catch)
if(BUILD_TESTING)
    if(APPLE)
        # On macOS, only build tests for debug builds
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_subdirectory(test/cpp)
        endif()
    elseif(CMAKE_CROSSCOMPILING)
        # Skip running tests when cross-compiling
        message(STATUS "Skipping tests during cross-compilation")
    else()
        # On other platforms (Linux), build tests for all configurations
        add_subdirectory(test/cpp)
    endif()
endif()

# Integration tests
add_custom_target(integration_tests
    COMMAND ${CMAKE_COMMAND} -E env "FLAPI_BUILD_TYPE=$<CONFIG>" uv sync
    COMMAND ${CMAKE_COMMAND} -E env "FLAPI_BUILD_TYPE=$<CONFIG>" uv run python -m pytest "*.tavern.yaml" -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test/integration
    DEPENDS flapi
)

# Windows-specific linker flags
if(WIN32)
    if(MSVC)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:CONSOLE")
        add_compile_options(/W4 /MP)
        # Enable parallel compilation
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP /Gy")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /Gy")
    elseif(MINGW)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--subsystem,console")
    endif()
endif()

# Add dependency on web UI
#add_dependencies(flapi-lib web_ui)

# Include embedded UI headers
target_include_directories(flapi-lib PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

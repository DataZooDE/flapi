cmake_minimum_required(VERSION 3.15)

# Set vcpkg toolchain before project()
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
    if (NOT CMAKE_LINUX_ARCHITECTURES)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(CMAKE_LINUX_ARCHITECTURES ${HOST_ARCH})
    endif()

    # Set vcpkg triplet based on architecture
    if(CMAKE_LINUX_ARCHITECTURES MATCHES "arm64")
        set(VCPKG_TARGET_TRIPLET "arm64-linux")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-linux")
    endif()
endif()

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" 
    CACHE STRING "Vcpkg toolchain file")

# Set project name and version
project(flAPI VERSION 0.1.0 LANGUAGES CXX)

# Basic settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Disable unused parameter warnings but keep other warnings
    set(COMMON_FLAGS "-Wall -Wextra -Wno-unused-parameter")
    
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
endif()

option(ENABLE_SANITIZER "Enable address sanitizer." TRUE)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer." FALSE)
option(ENABLE_UBSAN "Enable undefined behavior sanitizer." TRUE)

# Find dependencies
find_package(Threads REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(AWSSDK CONFIG COMPONENTS core secretsmanager REQUIRED)

# Add DuckDB
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

# Create flapi-lib
add_library(flapi-lib STATIC
    src/api_server.cpp
    src/auth_middleware.cpp
    src/cache_manager.cpp
    src/config_manager.cpp
    src/database_manager.cpp
    src/heartbeat_worker.cpp
    src/open_api_doc_generator.cpp
    src/request_handler.cpp
    src/request_validator.cpp
    src/rate_limit_middleware.cpp
    src/route_translator.cpp
    src/sql_template_processor.cpp   
)

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

# Tests
include(CTest)
include(Catch)
if(BUILD_TESTING)
    if(APPLE)
        # On macOS, only build tests for debug builds
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_subdirectory(test/cpp)
        endif()
    else()
        # On other platforms (Linux), build tests for all configurations
        add_subdirectory(test/cpp)
    endif()
endif()

# Integration tests
add_custom_target(integration_tests
    COMMAND npm install
    COMMAND npm test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test/integration
    DEPENDS flapi
)

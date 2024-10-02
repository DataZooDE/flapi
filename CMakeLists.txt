cmake_minimum_required(VERSION 3.15)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake)
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/x64-linux/share")

project(flAPI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Address Sanitizer only for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()

# Add flags to speed up build process
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")

# Disable link-time optimization
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

# Find other dependencies
find_package(Catch2 CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(inja CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Add DuckDB as a subdirectory
add_subdirectory(duckdb EXCLUDE_FROM_ALL)

# Include directories
find_path(JWT_CPP_INCLUDE_DIRS "jwt-cpp/base.h")
include_directories( 
    src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include
    ${CROW_INCLUDE_DIRS}
    ${JWT_CPP_INCLUDE_DIRS}
)

# Update the flapi executable
add_executable(flapi 
    src/main.cpp
    src/api_server.cpp
    src/config_manager.cpp
    src/database_manager.cpp
    src/cache_manager.cpp
    src/request_handler.cpp
    src/rate_limit_middleware.cpp
    src/auth_middleware.cpp
    src/route_translator.cpp
    src/sql_template_processor.cpp
    src/open_api_doc_generator.cpp
)

# Link libraries
target_link_libraries(flapi PRIVATE 
    duckdb_static
    Crow::Crow
    pantor::inja
    yaml-cpp::yaml-cpp
    argparse::argparse
    OpenSSL::Crypto
    OpenSSL::SSL
)

set_target_properties(flapi PROPERTIES ENABLE_EXPORTS TRUE)

# Add tests
#add_subdirectory(test)

cmake_minimum_required(VERSION 3.15)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake)
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/x64-linux/share")

project(flAPI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    endif()
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fsanitize-undefined-trap-on-error -Os -DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined -fsanitize-undefined-trap-on-error")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")

if(WIN32)
    set(CMAKE_GENERATOR "Visual Studio 16 2019")
endif()

# Find other dependencies
find_package(Catch2 CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
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
target_link_libraries(flapi-lib PRIVATE 
    duckdb_static
    Crow::Crow
    yaml-cpp::yaml-cpp
    argparse::argparse
    OpenSSL::Crypto
    OpenSSL::SSL
)

# Update the flapi executable
add_executable(flapi src/main.cpp)

# Link flapi-lib to the main executable
target_link_libraries(flapi PRIVATE flapi-lib)
target_link_options(flapi PRIVATE -fsanitize=undefined)
set_target_properties(flapi PROPERTIES ENABLE_EXPORTS TRUE)

# Add tests
include(CTest)
include(Catch)
add_subdirectory(test/cpp)

# Add integration tests
add_custom_target(integration_tests
    COMMAND npm install
    COMMAND npm test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test/integration
    DEPENDS flapi
)

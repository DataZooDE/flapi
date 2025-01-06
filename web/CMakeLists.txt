# Web UI CMake configuration
cmake_minimum_required(VERSION 3.15)
project(web_ui)

# Find npm executable
find_program(NPM_EXECUTABLE npm REQUIRED)

# Create custom target for web UI
add_custom_target(web_ui ALL
    # Clean previous build
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_SOURCE_DIR}/dist
    
    # Install dependencies and build
    COMMAND ${NPM_EXECUTABLE} install
    COMMAND ${NPM_EXECUTABLE} run build
    
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building web UI..."
)

# Generate embedded UI header
add_custom_command(
    TARGET web_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/dist/index.html
        -DOUTPUT_DIR=${CMAKE_SOURCE_DIR}/src/include/embedded
        -P ${CMAKE_CURRENT_SOURCE_DIR}/generate_embedded_ui.cmake
    COMMENT "Generating embedded UI header..."
)

# Create interface library for the embedded UI
add_library(web_ui_lib INTERFACE)
target_include_directories(web_ui_lib INTERFACE 
    ${CMAKE_SOURCE_DIR}/src/include
) 
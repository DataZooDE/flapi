# Script to convert web UI build output to C++ header

# Ensure required variables are set
if(NOT DEFINED SOURCE_FILE)
    message(FATAL_ERROR "SOURCE_FILE not defined")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR not defined")
endif()

# Create output directory if it doesn't exist
file(MAKE_DIRECTORY ${OUTPUT_DIR})

# Read the HTML file
file(READ ${SOURCE_FILE} FILE_CONTENT HEX)

# Generate the header file content
set(HEADER_CONTENT "#pragma once

namespace flapi {
namespace embedded_ui {

// Embedded web UI content
const unsigned char index_html[] = {
")

# Convert hex content to C array format
string(REGEX MATCHALL "([A-Fa-f0-9][A-Fa-f0-9])" BYTES ${FILE_CONTENT})
set(BYTE_ARRAY "")
set(COUNT 0)
foreach(BYTE ${BYTES})
    if(COUNT EQUAL 0)
        set(BYTE_ARRAY "${BYTE_ARRAY}    ")
    endif()
    set(BYTE_ARRAY "${BYTE_ARRAY}0x${BYTE}, ")
    math(EXPR COUNT "(${COUNT} + 1) % 12")
    if(COUNT EQUAL 0)
        set(BYTE_ARRAY "${BYTE_ARRAY}\n")
    endif()
endforeach()

# Add array size and closing content
string(LENGTH "${FILE_CONTENT}" CONTENT_LENGTH)
set(HEADER_CONTENT "${HEADER_CONTENT}${BYTE_ARRAY}
};

const unsigned int index_html_len = ${CONTENT_LENGTH};

} // namespace embedded_ui
} // namespace flapi
")

# Write the header file
file(WRITE ${OUTPUT_DIR}/index_html.hpp "${HEADER_CONTENT}") 
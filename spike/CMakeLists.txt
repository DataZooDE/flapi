# Issue -1 link spike. Standalone: builds the throwaway TU against the SAME
# vcpkg-installed opentelemetry-cpp the real build would use, with the SAME
# linker flags flapi uses, so a link failure here is a real link failure.
cmake_minimum_required(VERSION 3.21)
project(otel_link_spike CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(opentelemetry-cpp CONFIG REQUIRED)

add_executable(otel_link_spike otel_link_spike.cpp)
target_link_libraries(otel_link_spike PRIVATE
    opentelemetry-cpp::trace
    opentelemetry-cpp::resources
    opentelemetry-cpp::otlp_http_exporter)

# flapi's Linux link flags — --no-undefined turns any ordering problem into a
# hard failure rather than a lazy runtime one.
if(UNIX AND NOT APPLE)
    target_link_options(otel_link_spike PRIVATE -Wl,--as-needed -Wl,--no-undefined)
endif()

message(STATUS "opentelemetry-cpp version: ${opentelemetry-cpp_VERSION}")

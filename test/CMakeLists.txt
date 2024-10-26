add_subdirectory(cpp)

# Add integration tests
add_dependencies(check integration_tests)

find_package(Catch2 3 REQUIRED)

add_executable(flapi_tests
    main.cpp
    auth_middleware_test.cpp
    config_manager_test.cpp
    config_manager_yaml_validation_test.cpp
    config_manager_path_resolution_test.cpp
    config_service_test.cpp
    config_service_filesystem_test.cpp
    config_service_parameters_test.cpp
    config_service_slug_test.cpp
    config_service_template_lookup_test.cpp
    config_service_schema_test.cpp
    database_manager_test.cpp
    endpoint_config_parser_test.cpp
    extended_yaml_parser_test.cpp
    https_config_test.cpp
    mcp_prompt_handler_test.cpp
    query_executor_test.cpp
    rate_limit_middleware_test.cpp
    request_validator_test.cpp
    sql_template_processor_test.cpp
    test_duckdb_raii.cpp
    test_json_utils.cpp
    test_config_loader.cpp
    test_endpoint_repository.cpp
    test_config_validator.cpp
    test_config_serializer.cpp
    test_error.cpp
    test_type_converter.cpp
    test_arrow_integration.cpp
    test_content_negotiation.cpp
    test_arrow_serialization.cpp
    test_arrow_compression.cpp
)

target_include_directories(flapi_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/test/cpp/include
)

target_link_libraries(flapi_tests PRIVATE 
    flapi-lib
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(flapi_tests 
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES 
        ENVIRONMENT "DYLD_LIBRARY_PATH=${CMAKE_BINARY_DIR}"
        SKIP_RETURN_CODE 4
    TEST_SUFFIX "_test"
)
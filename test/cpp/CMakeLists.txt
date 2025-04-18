find_package(Catch2 3 REQUIRED)

add_executable(flapi_tests
    main.cpp
    auth_middleware_test.cpp
    config_manager_test.cpp
    config_service_test.cpp
    database_manager_test.cpp
    query_executor_test.cpp
    request_validator_test.cpp
    sql_template_processor_test.cpp
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
find_package(Catch2 3 REQUIRED)

add_executable(flapi_tests
    main_test.cpp
    config_manager_test.cpp
    request_validator_test.cpp
    sql_template_processor_test.cpp
    auth_middleware_test.cpp
    database_manager_test.cpp
)

target_include_directories(flapi_tests PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/include
)

target_link_libraries(flapi_tests PRIVATE 
    flapi-lib
    Catch2::Catch2WithMain
    yaml-cpp::yaml-cpp
    jwt-cpp::jwt-cpp
    OpenSSL::Crypto
    OpenSSL::SSL
    ${AWSSDK_LIBRARIES}
)
target_link_options(flapi_tests PRIVATE -fsanitize=undefined)

include(CTest)
include(Catch)
catch_discover_tests(flapi_tests)

# Add the test to CTest
add_test(NAME flapi_tests COMMAND flapi_tests)
find_package(Catch2 3 REQUIRED)

add_executable(flapi_tests
    main_test.cpp
    config_manager_test.cpp
)

target_include_directories(flapi_tests PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/include
)

target_link_libraries(flapi_tests PUBLIC flapi Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(flapi_tests)
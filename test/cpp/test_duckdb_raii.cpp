#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "duckdb_raii.hpp"
#include <cstring>
#include <stdexcept>

using namespace flapi;

// Helper function to allocate a test string like DuckDB would
char* allocate_test_string(const std::string& str) {
    char* ptr = static_cast<char*>(duckdb_malloc(str.length() + 1));
    std::strcpy(ptr, str.c_str());
    return ptr;
}

TEST_CASE("DuckDBString RAII", "[memory][raii]") {
    SECTION("Automatic cleanup on scope exit") {
        {
            char* test_str = allocate_test_string("test");
            DuckDBString wrapped(test_str);
            REQUIRE(wrapped.to_string() == "test");
            REQUIRE(wrapped.get() != nullptr);
        }
        // test_str automatically freed - no way to verify, but if we crash it failed
    }

    SECTION("get() returns correct pointer") {
        char* test_str = allocate_test_string("hello");
        DuckDBString wrapped(test_str);
        REQUIRE(std::string(wrapped.get()) == "hello");
    }

    SECTION("to_string() creates std::string") {
        char* test_str = allocate_test_string("world");
        DuckDBString wrapped(test_str);
        std::string result = wrapped.to_string();
        REQUIRE(result == "world");
        // Verify it's a copy, not a reference
        REQUIRE(result.c_str() != wrapped.get());
    }

    SECTION("Handles null pointer") {
        DuckDBString wrapped(nullptr);
        REQUIRE(wrapped.get() == nullptr);
        REQUIRE(wrapped.to_string() == "");
        REQUIRE(wrapped.is_null());
    }

    SECTION("Move constructor transfers ownership") {
        char* test_str = allocate_test_string("moved");
        {
            DuckDBString s1(test_str);
            REQUIRE(s1.get() != nullptr);
            REQUIRE(s1.to_string() == "moved");

            DuckDBString s2 = std::move(s1);
            REQUIRE(s2.get() != nullptr);
            REQUIRE(s2.to_string() == "moved");
            REQUIRE(s1.get() == nullptr);  // s1 should be null after move
            REQUIRE(s1.is_null());
        }
        // s2 destroyed, should free the pointer
    }

    SECTION("Move assignment transfers ownership") {
        char* test_str1 = allocate_test_string("first");
        char* test_str2 = allocate_test_string("second");

        {
            DuckDBString s1(test_str1);
            DuckDBString s2(test_str2);

            REQUIRE(s1.to_string() == "first");
            REQUIRE(s2.to_string() == "second");

            s1 = std::move(s2);

            REQUIRE(s1.to_string() == "second");
            REQUIRE(s2.get() == nullptr);
        }
        // Both cleaned up properly
    }

    SECTION("Move assignment to self is safe") {
        char* test_str = allocate_test_string("self");
        DuckDBString s(test_str);

        // This should not cause double-free (suppress warning for self-move)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
        s = std::move(s);
#pragma GCC diagnostic pop

        REQUIRE(s.to_string() == "self");
    }

    SECTION("Copy is deleted") {
        char* test_str = allocate_test_string("nocopy");
        DuckDBString s(test_str);

        // This should not compile if uncommented:
        // DuckDBString s2 = s;  // Compile error: deleted copy constructor
        // s2 = s;               // Compile error: deleted copy assignment
    }

    SECTION("Exception safety") {
        bool exception_thrown = false;
        try {
            char* test_str = allocate_test_string("exception");
            DuckDBString s(test_str);
            throw std::runtime_error("test exception");
        } catch (const std::runtime_error&) {
            exception_thrown = true;
        }
        // Memory should be cleaned up even though exception was thrown
        REQUIRE(exception_thrown);
    }

    SECTION("Multiple strings in same scope") {
        char* str1 = allocate_test_string("one");
        char* str2 = allocate_test_string("two");
        char* str3 = allocate_test_string("three");

        {
            DuckDBString s1(str1);
            DuckDBString s2(str2);
            DuckDBString s3(str3);

            REQUIRE(s1.to_string() == "one");
            REQUIRE(s2.to_string() == "two");
            REQUIRE(s3.to_string() == "three");
        }
        // All three freed
    }

    SECTION("Empty string") {
        char* empty = allocate_test_string("");
        DuckDBString s(empty);
        REQUIRE(s.to_string() == "");
        REQUIRE(s.get() != nullptr);
    }

    SECTION("String with special characters") {
        std::string special = "test\"'\\n\t";
        char* str = allocate_test_string(special);
        DuckDBString s(str);
        REQUIRE(s.to_string() == special);
    }
}

TEST_CASE("DuckDBResult RAII", "[memory][raii]") {
    SECTION("Default construction creates uninitialized result") {
        DuckDBResult result;
        REQUIRE(!result.has_result());
    }

    SECTION("Handles uninitialized result gracefully") {
        {
            DuckDBResult result;
            // Don't initialize, just let it go out of scope
        }
        // Should not crash or access uninitialized memory
    }


    SECTION("get() returns pointer to result") {
        DuckDBResult result;
        duckdb_result* ptr = result.get();
        REQUIRE(ptr != nullptr);
        REQUIRE(ptr == result.get());  // Same pointer on multiple calls
    }

    SECTION("const get() returns const pointer") {
        const DuckDBResult result;
        const duckdb_result* ptr = result.get();
        REQUIRE(ptr != nullptr);
    }

    SECTION("Move constructor transfers ownership with real query") {
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult r1;
            duckdb_state state = duckdb_query(conn, "SELECT 1", r1.get());
            if (state == DuckDBSuccess) {
                r1.set_initialized();
                REQUIRE(r1.has_result());

                DuckDBResult r2 = std::move(r1);
                REQUIRE(r2.has_result());
                REQUIRE(!r1.has_result());  // r1 should be uninitialized after move
            }
            // r2 destroyed, should clean up
        }

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }

    SECTION("Move assignment transfers ownership with real query") {
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult r1;
            duckdb_state state1 = duckdb_query(conn, "SELECT 1", r1.get());

            DuckDBResult r2;
            duckdb_state state2 = duckdb_query(conn, "SELECT 2", r2.get());

            if (state1 == DuckDBSuccess && state2 == DuckDBSuccess) {
                r1.set_initialized();
                r2.set_initialized();

                r1 = std::move(r2);

                REQUIRE(r1.has_result());
                REQUIRE(!r2.has_result());
            }
        }

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }

    SECTION("Copy is deleted") {
        DuckDBResult r;

        // This should not compile if uncommented:
        // DuckDBResult r2 = r;  // Compile error: deleted copy constructor
        // r2 = r;               // Compile error: deleted copy assignment
    }

    SECTION("Exception safety with initialization and real query") {
        bool exception_thrown = false;
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        try {
            DuckDBResult result;
            duckdb_state state = duckdb_query(conn, "SELECT 1", result.get());
            if (state == DuckDBSuccess) {
                result.set_initialized();
                throw std::runtime_error("test exception");
            }
        } catch (const std::runtime_error&) {
            exception_thrown = true;
        }
        // Should not crash during cleanup
        REQUIRE(exception_thrown);

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }

    SECTION("Multiple results in same scope with real queries") {
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult r1;
            DuckDBResult r2;
            DuckDBResult r3;

            duckdb_state state1 = duckdb_query(conn, "SELECT 1", r1.get());
            duckdb_state state2 = duckdb_query(conn, "SELECT 2", r2.get());

            if (state1 == DuckDBSuccess) {
                r1.set_initialized();
            }
            if (state2 == DuckDBSuccess) {
                r2.set_initialized();
            }
            // r3 left uninitialized

            REQUIRE(r1.has_result());
            REQUIRE(r2.has_result());
            REQUIRE(!r3.has_result());
        }
        // All cleaned up properly

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }

    SECTION("Real DuckDB query lifecycle") {
        duckdb_database db;
        duckdb_connection conn;

        // Open database and connection
        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult result;

            // Execute a simple query
            duckdb_state state = duckdb_query(conn, "SELECT 1 AS answer", result.get());
            if (state == DuckDBSuccess) {
                result.set_initialized();
                REQUIRE(result.has_result());
            }
            // result automatically destroyed on scope exit
        }

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }

    SECTION("Query with error handling") {
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult result;

            // Execute invalid query
            duckdb_state state = duckdb_query(conn, "SELECT * FROM nonexistent", result.get());

            // Even if query fails, result should be cleanable
            if (state != DuckDBSuccess) {
                // Don't set initialized, as query failed
                REQUIRE(!result.has_result());
            }
        }

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }
}

TEST_CASE("Integration: DuckDBString and DuckDBResult together", "[memory][raii]") {
    SECTION("Extract strings from result") {
        duckdb_database db;
        duckdb_connection conn;

        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

        {
            DuckDBResult result;
            duckdb_state state = duckdb_query(conn, "SELECT 'hello' AS greeting", result.get());
            if (state == DuckDBSuccess) {
                result.set_initialized();

                // Extract column name as DuckDB string
                const char* col_name_const = duckdb_column_name(result.get(), 0);
                if (col_name_const) {
                    // Need to allocate our own copy since duckdb_column_name returns const char*
                    char* col_name_copy = static_cast<char*>(duckdb_malloc(strlen(col_name_const) + 1));
                    strcpy(col_name_copy, col_name_const);
                    {
                        DuckDBString name_wrapper(col_name_copy);
                        REQUIRE(name_wrapper.to_string() == "greeting");
                    }
                    // name_wrapper destroyed and memory freed
                }
            }
            // result destroyed and cleaned up
        }

        duckdb_disconnect(&conn);
        duckdb_close(&db);
    }
}

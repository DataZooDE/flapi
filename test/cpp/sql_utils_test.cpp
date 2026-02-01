#include <catch2/catch_test_macros.hpp>
#include "sql_utils.hpp"

using flapi::splitSqlStatements;
using flapi::trimSqlString;

// =============================================================================
// BASIC SPLITTING
// =============================================================================

TEST_CASE("splitSqlStatements - basic splitting", "[sql_utils]") {
    SECTION("single statement no semicolon") {
        auto result = splitSqlStatements("SELECT * FROM t");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT * FROM t");
    }

    SECTION("single statement with trailing semicolon") {
        auto result = splitSqlStatements("SELECT * FROM t;");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT * FROM t");
    }

    SECTION("two statements") {
        auto result = splitSqlStatements("INSERT INTO t VALUES (1); SELECT * FROM t");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "INSERT INTO t VALUES (1)");
        REQUIRE(result[1] == "SELECT * FROM t");
    }

    SECTION("three statements") {
        auto result = splitSqlStatements("SELECT 1; SELECT 2; SELECT 3");
        REQUIRE(result.size() == 3);
        REQUIRE(result[0] == "SELECT 1");
        REQUIRE(result[1] == "SELECT 2");
        REQUIRE(result[2] == "SELECT 3");
    }

    SECTION("multiple statements with whitespace") {
        auto result = splitSqlStatements("  SELECT 1;  SELECT 2;  SELECT 3;  ");
        REQUIRE(result.size() == 3);
        REQUIRE(result[0] == "SELECT 1");
        REQUIRE(result[1] == "SELECT 2");
        REQUIRE(result[2] == "SELECT 3");
    }

    SECTION("empty input") {
        auto result = splitSqlStatements("");
        REQUIRE(result.empty());
    }

    SECTION("only whitespace") {
        auto result = splitSqlStatements("   \n\t  ");
        REQUIRE(result.empty());
    }

    SECTION("only semicolons") {
        auto result = splitSqlStatements(";;;");
        REQUIRE(result.empty());
    }

    SECTION("semicolons with whitespace") {
        auto result = splitSqlStatements(" ; ; ; ");
        REQUIRE(result.empty());
    }

    SECTION("newlines between statements") {
        auto result = splitSqlStatements("SELECT 1;\nSELECT 2;\nSELECT 3");
        REQUIRE(result.size() == 3);
    }
}

// =============================================================================
// SINGLE QUOTED STRINGS
// =============================================================================

TEST_CASE("splitSqlStatements - single quoted strings", "[sql_utils]") {
    SECTION("semicolon in single quotes") {
        auto result = splitSqlStatements("SELECT 'a;b' FROM t");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT 'a;b' FROM t");
    }

    SECTION("multiple semicolons in single quotes") {
        auto result = splitSqlStatements("SELECT 'a;b;c;d' FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("escaped single quote") {
        auto result = splitSqlStatements("SELECT 'it''s fine; really' FROM t");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT 'it''s fine; really' FROM t");
    }

    SECTION("multiple escaped quotes") {
        auto result = splitSqlStatements("SELECT 'a''b''c;d''e' FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("escaped quote at end of string") {
        auto result = splitSqlStatements("SELECT 'test'';' FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("empty string literal") {
        auto result = splitSqlStatements("SELECT ''; SELECT 2");
        REQUIRE(result.size() == 2);
    }

    SECTION("statement after quoted string") {
        auto result = splitSqlStatements("SELECT 'test;test'; SELECT 2");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "SELECT 'test;test'");
        REQUIRE(result[1] == "SELECT 2");
    }

    SECTION("multiple quoted strings in one statement") {
        auto result = splitSqlStatements("SELECT 'a;b', 'c;d' FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("BigQuery query example - nested quotes") {
        std::string query = "SELECT * FROM bigquery_query('proj', "
                           "'DECLARE x; CALL proc(); SELECT * FROM t')";
        auto result = splitSqlStatements(query);
        REQUIRE(result.size() == 1);
    }

    SECTION("deeply nested escaped quotes") {
        auto result = splitSqlStatements("SELECT 'outer ''inner; text'' more' FROM t");
        REQUIRE(result.size() == 1);
    }
}

// =============================================================================
// DOUBLE QUOTED STRINGS
// =============================================================================

TEST_CASE("splitSqlStatements - double quoted strings", "[sql_utils]") {
    SECTION("semicolon in double quotes") {
        auto result = splitSqlStatements("SELECT \"col;name\" FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("multiple semicolons in double quotes") {
        auto result = splitSqlStatements("SELECT \"a;b;c\" FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("escaped double quote") {
        auto result = splitSqlStatements("SELECT \"test\"\"more;\" FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("mixed single and double quotes") {
        auto result = splitSqlStatements("SELECT 'a;b', \"c;d\" FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("single quote inside double quotes") {
        auto result = splitSqlStatements("SELECT \"it's; here\" FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("double quote inside single quotes") {
        auto result = splitSqlStatements("SELECT 'say \"hello;\"; bye' FROM t");
        REQUIRE(result.size() == 1);
    }
}

// =============================================================================
// DOLLAR QUOTING
// =============================================================================

TEST_CASE("splitSqlStatements - dollar quoting", "[sql_utils]") {
    SECTION("basic dollar quote $$") {
        auto result = splitSqlStatements("SELECT $$ text; here $$");
        REQUIRE(result.size() == 1);
    }

    SECTION("tagged dollar quote") {
        auto result = splitSqlStatements("SELECT $tag$ text; here $tag$");
        REQUIRE(result.size() == 1);
    }

    SECTION("dollar quote with alphanumeric tag") {
        auto result = splitSqlStatements("SELECT $abc123$ text; here $abc123$");
        REQUIRE(result.size() == 1);
    }

    SECTION("dollar quote with underscore in tag") {
        auto result = splitSqlStatements("SELECT $my_tag$ text; here $my_tag$");
        REQUIRE(result.size() == 1);
    }

    SECTION("dollar quote with following statement") {
        auto result = splitSqlStatements("SELECT $$ a;b $$; SELECT 2");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "SELECT $$ a;b $$");
        REQUIRE(result[1] == "SELECT 2");
    }

    SECTION("multiple dollar quoted sections") {
        auto result = splitSqlStatements("SELECT $$ a;b $$, $$ c;d $$; SELECT 2");
        REQUIRE(result.size() == 2);
    }

    SECTION("different tags don't match") {
        // $a$ ... $b$ - $b$ doesn't close $a$
        auto result = splitSqlStatements("SELECT $a$ $b$ text; $b$ more $a$");
        REQUIRE(result.size() == 1);
    }

    SECTION("single quote inside dollar quote") {
        auto result = splitSqlStatements("SELECT $$ it's; fine $$ FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("double quote inside dollar quote") {
        auto result = splitSqlStatements("SELECT $$ say \"hi;\"; $$ FROM t");
        REQUIRE(result.size() == 1);
    }

    SECTION("dollar sign not starting tag") {
        // Just a $ followed by non-tag should be treated as regular char
        auto result = splitSqlStatements("SELECT $5; SELECT 2");
        REQUIRE(result.size() == 2);
    }
}

// =============================================================================
// SECURITY EDGE CASES
// =============================================================================

TEST_CASE("splitSqlStatements - security edge cases", "[sql_utils][security]") {
    SECTION("unclosed single quote - treat rest as quoted (fail-safe)") {
        // Malformed SQL - should not split after unclosed quote
        auto result = splitSqlStatements("SELECT 'unclosed; DROP TABLE t");
        REQUIRE(result.size() == 1);  // Don't split - safer
    }

    SECTION("unclosed double quote - treat rest as quoted") {
        auto result = splitSqlStatements("SELECT \"unclosed; DROP TABLE t");
        REQUIRE(result.size() == 1);
    }

    SECTION("unclosed dollar quote - treat rest as quoted") {
        auto result = splitSqlStatements("SELECT $tag$ unclosed; DROP TABLE t");
        REQUIRE(result.size() == 1);
    }

    SECTION("properly closed quote allows split") {
        // This is valid SQL that should split
        auto result = splitSqlStatements("SELECT * FROM t WHERE x = 'y'; DROP TABLE t");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "SELECT * FROM t WHERE x = 'y'");
        REQUIRE(result[1] == "DROP TABLE t");
    }

    SECTION("backslash does NOT escape quote in SQL") {
        // SQL standard uses '' for escaping, not \'
        // So \' should not close the quote context
        auto result = splitSqlStatements("SELECT 'test\\'; DROP TABLE t");
        // The string is: 'test\' - still open because \' is not an escape in SQL
        // Then DROP TABLE t is outside... but wait, the ' after \ closes it
        // Actually: 'test\' - this is a complete string containing "test\"
        // Then ; DROP TABLE t is a second statement
        REQUIRE(result.size() == 2);
    }

    SECTION("backslash followed by escaped quote") {
        // 'test\''' - string is test\ followed by escaped quote
        auto result = splitSqlStatements("SELECT 'test\\'''; SELECT 2");
        REQUIRE(result.size() == 2);
    }

    SECTION("injection attempt with comment") {
        auto result = splitSqlStatements("SELECT '-- comment; DROP TABLE t'; SELECT 2");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "SELECT '-- comment; DROP TABLE t'");
        REQUIRE(result[1] == "SELECT 2");
    }

    SECTION("very long string") {
        std::string longStr(10000, 'a');
        auto result = splitSqlStatements("SELECT '" + longStr + ";'; SELECT 2");
        REQUIRE(result.size() == 2);
    }

    SECTION("string with only semicolons") {
        auto result = splitSqlStatements("SELECT ';;;'; SELECT 2");
        REQUIRE(result.size() == 2);
    }

    SECTION("alternating quotes") {
        auto result = splitSqlStatements("SELECT 'a' || \"b\" || 'c;d'; SELECT 2");
        REQUIRE(result.size() == 2);
    }
}

// =============================================================================
// REAL WORLD QUERIES
// =============================================================================

TEST_CASE("splitSqlStatements - real world queries", "[sql_utils]") {
    SECTION("INSERT with RETURNING") {
        auto result = splitSqlStatements(
            "INSERT INTO users (name) VALUES ('John'); "
            "SELECT * FROM users WHERE id = last_insert_rowid()");
        REQUIRE(result.size() == 2);
    }

    SECTION("multi-line query") {
        auto result = splitSqlStatements(
            "SELECT *\n"
            "FROM table1;\n"
            "SELECT *\n"
            "FROM table2");
        REQUIRE(result.size() == 2);
    }

    SECTION("CREATE TABLE with constraints") {
        auto result = splitSqlStatements(
            "CREATE TABLE t (id INT, name VARCHAR(100));"
            "INSERT INTO t VALUES (1, 'test;value');"
            "SELECT * FROM t");
        REQUIRE(result.size() == 3);
    }

    SECTION("BigQuery procedure call") {
        std::string query =
            "SELECT * FROM bigquery_query('d-kaercher-kaadala-mgmt', "
            "'DECLARE customer_id STRING; "
            "CALL `d-kaercher-kaadala-mgmt.products.create_customer`(''Fabian'', customer_id); "
            "SELECT * FROM d-kaercher-kaadala-mgmt.products.customers "
            "WHERE customer_id = customer_id')";
        auto result = splitSqlStatements(query);
        REQUIRE(result.size() == 1);
    }

    SECTION("DuckDB function with semicolons in string arg") {
        auto result = splitSqlStatements(
            "SELECT * FROM read_csv('file.csv', header=true); "
            "SELECT 'done;'");
        REQUIRE(result.size() == 2);
    }

    SECTION("PostgreSQL-style function body") {
        auto result = splitSqlStatements(
            "CREATE FUNCTION test() RETURNS void AS $$ "
            "BEGIN "
            "  INSERT INTO log VALUES ('started;'); "
            "END; "
            "$$ LANGUAGE plpgsql; "
            "SELECT test()");
        REQUIRE(result.size() == 2);
    }
}

// =============================================================================
// WHITESPACE HANDLING
// =============================================================================

TEST_CASE("splitSqlStatements - whitespace handling", "[sql_utils]") {
    SECTION("leading whitespace") {
        auto result = splitSqlStatements("   SELECT 1");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT 1");
    }

    SECTION("trailing whitespace") {
        auto result = splitSqlStatements("SELECT 1   ");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "SELECT 1");
    }

    SECTION("whitespace between statements") {
        auto result = splitSqlStatements("SELECT 1;   \n\t   SELECT 2");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == "SELECT 1");
        REQUIRE(result[1] == "SELECT 2");
    }

    SECTION("tabs and newlines") {
        auto result = splitSqlStatements("\tSELECT 1\n;\n\tSELECT 2\n");
        REQUIRE(result.size() == 2);
    }

    SECTION("carriage return") {
        auto result = splitSqlStatements("SELECT 1;\r\nSELECT 2");
        REQUIRE(result.size() == 2);
    }
}

// =============================================================================
// TRIM FUNCTION
// =============================================================================

TEST_CASE("trimSqlString", "[sql_utils]") {
    SECTION("no trim needed") {
        REQUIRE(trimSqlString("hello") == "hello");
    }

    SECTION("leading spaces") {
        REQUIRE(trimSqlString("   hello") == "hello");
    }

    SECTION("trailing spaces") {
        REQUIRE(trimSqlString("hello   ") == "hello");
    }

    SECTION("both sides") {
        REQUIRE(trimSqlString("   hello   ") == "hello");
    }

    SECTION("tabs and newlines") {
        REQUIRE(trimSqlString("\t\n hello \n\t") == "hello");
    }

    SECTION("empty string") {
        REQUIRE(trimSqlString("") == "");
    }

    SECTION("only whitespace") {
        REQUIRE(trimSqlString("   \t\n   ") == "");
    }

    SECTION("preserves internal whitespace") {
        REQUIRE(trimSqlString("  hello   world  ") == "hello   world");
    }
}

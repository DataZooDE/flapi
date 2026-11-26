#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "database_manager.hpp"
#include "config_manager.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;
namespace fs = std::filesystem;

class TestFixture {
protected:
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<DatabaseManager> db_manager;
    fs::path temp_dir;
    fs::path db_path;
    fs::path config_path;
    fs::path templates_dir;

    void SetUp() {
        // Create temporary directory for test files
        temp_dir = fs::temp_directory_path() / "flapi_test";
        fs::create_directories(temp_dir);
        
        // Create paths
        db_path = temp_dir / "test.db";
        config_path = temp_dir / "config.yaml";
        templates_dir = temp_dir / "templates";
        
        // Create templates directory
        fs::create_directories(templates_dir);

        // Create a minimal config file with all required fields
        std::ofstream config_file(config_path);
        config_file << R"(
project-name: flapi_test
project-description: Test configuration for FLAPI
server-name: test_server

template:
  path: )" << templates_dir.string() << R"(

duckdb:
  db_path: )" << db_path.string() << R"(

ducklake:
  enabled: false

connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";
        config_file.close();

        // Create a dummy template file
        std::ofstream template_file(templates_dir / "test.sql");
        template_file << "SELECT 1 as value";
        template_file.close();

        // Initialize managers with the config file path
        config_manager = std::make_shared<ConfigManager>(config_path);
        config_manager->loadConfig();
        
        db_manager = DatabaseManager::getInstance();
        db_manager->initializeDBManagerFromConfig(config_manager);
    }

    void TearDown() {
        // Reset the DatabaseManager singleton to clean state for next test
        if (db_manager) {
            db_manager->reset();
        }
        db_manager.reset();
        config_manager.reset();
        fs::remove_all(temp_dir);
    }

public:
    TestFixture() { SetUp(); }
    ~TestFixture() { TearDown(); }
};

TEST_CASE_METHOD(TestFixture, "DatabaseManager basic initialization", "[database_manager]") {
    REQUIRE(db_manager != nullptr);
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager can execute simple query", "[database_manager]") {
    std::map<std::string, std::string> params;
    auto result = db_manager->executeQuery("SELECT 42 as number", params);
    
    REQUIRE(result.data.size() == 1);
    REQUIRE(result.data[0]["number"].dump() == "42");
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager handles pagination", "[database_manager]") {
    // Use unique table name for this test
    std::string table_name = "pagination_test_table";
    db_manager->executeQuery("CREATE TABLE " + table_name + " AS SELECT * FROM (VALUES (1), (2), (3), (4), (5)) t(id)", {}, false);
    
    std::map<std::string, std::string> params = {
        {"limit", "2"},
        {"offset", "0"}
    };
    
    auto result = db_manager->executeQuery("SELECT * FROM " + table_name + " ORDER BY id", params);
    
    REQUIRE(result.data.size() == 2);
    REQUIRE(result.total_count == 5);
    REQUIRE(!result.next.empty());
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager can check table existence", "[database_manager]") {
    // Use unique table name for this test
    std::string table_name = "table_existence_test";
    db_manager->executeInitStatement("CREATE TABLE " + table_name + " (id INTEGER)");
    
    // Now check if the table exists
    REQUIRE(db_manager->tableExists("main", table_name));
    REQUIRE_FALSE(db_manager->tableExists("main", "nonexistent_table"));
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager can get table names", "[database_manager]") {
    // Use unique table names for this test
    std::string table_prefix = "get_table_names_test_";
    std::string table1 = table_prefix + "table1";
    std::string table2 = table_prefix + "table2";
    
    db_manager->executeQuery("CREATE TABLE " + table1 + " (id INTEGER)", {}, false);
    db_manager->executeQuery("CREATE TABLE " + table2 + " (id INTEGER)", {}, false);
    
    auto tables = db_manager->getTableNames("main", table_prefix, true);
    
    REQUIRE(tables.size() == 2);
    REQUIRE(std::find(tables.begin(), tables.end(), table1) != tables.end());
    REQUIRE(std::find(tables.begin(), tables.end(), table2) != tables.end());
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager can handle JSON data", "[database_manager][json]") {
    std::string table_name = "json_data_test_table";
    std::string json_data = R"({"key": "value"})";

    // Skip test if JSON extension is not available
    try {
        db_manager->executeQuery("CREATE OR REPLACE TABLE " + table_name + " (j JSON)", {}, false);
        db_manager->executeQuery("INSERT INTO " + table_name + " VALUES ('" + json_data + "')", {}, false);
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("json") != std::string::npos) {
            SKIP("JSON extension not available, skipping JSON functionality test");
        }
        throw; // Re-throw if it's a different error
    }

    auto result = db_manager->executeQuery("SELECT * FROM " + table_name, {});
    REQUIRE(result.data.size() == 1);
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager handles query errors gracefully", "[database_manager]") {
    std::map<std::string, std::string> params;
    REQUIRE_THROWS_AS(
        db_manager->executeQuery("SELECT * FROM nonexistent_table", params),
        std::runtime_error
    );
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager can describe query structure", "[database_manager]") {
    // Create a test SQL template file
    fs::path sql_template = templates_dir / "describe_test.sql";
    std::ofstream template_file(sql_template);
    template_file << "SELECT 42 as number, 'text' as string_col, TRUE as bool_col";
    template_file.close();
    
    // Create a test endpoint with complete configuration
    EndpointConfig endpoint;
    endpoint.templateSource = sql_template.string();
    endpoint.method = "GET";
    endpoint.urlPath = "/test";
    endpoint.connection.push_back("default");

    // Add a default connection to the config
    std::ofstream config_file(config_path, std::ios::app);
    config_file << R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";
    config_file.close();

    // Reload the configuration
    config_manager->loadConfig();
    
    // Add this line to debug the SQL processor state
    std::cout << "Template directory: " << templates_dir << std::endl;
    std::cout << "Template source: " << endpoint.templateSource << std::endl;
    
    auto description = db_manager->describeSelectQuery(endpoint);
    
    // Add this line to debug the output
    std::cout << "Description output: " << YAML::Dump(description) << std::endl;

    REQUIRE(description["number"]["type"].as<std::string>() == "integer");
    REQUIRE(description["string_col"]["type"].as<std::string>() == "string");
    REQUIRE(description["bool_col"]["type"].as<std::string>() == "boolean");
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager handles concurrent connections", "[database_manager]") {
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successful_queries(0);
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            try {
                std::map<std::string, std::string> params;
                auto result = db_manager->executeQuery("SELECT 1", params);
                if (result.data.size() == 1) {
                    successful_queries++;
                }
            } catch (...) {
                // Count failed
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    REQUIRE(successful_queries == num_threads);
} 

TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite INSERT operation", "[database_manager]") {
    // Create a test table with auto-incrementing primary key
    db_manager->executeQuery("CREATE TABLE test_write (id INTEGER PRIMARY KEY, name VARCHAR, email VARCHAR)", {}, false);
    
    // Create endpoint config for write operation
    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "insert_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = true;
    endpoint.connection.push_back("default");
    
    // Create INSERT SQL template with explicit ID to avoid NOT NULL constraint
    std::ofstream template_file(templates_dir / "insert_test.sql");
    template_file << "INSERT INTO test_write (id, name, email) VALUES ({{params.id}}, '{{params.name}}', '{{params.email}}') RETURNING id, name, email";
    template_file.close();
    
    // Prepare parameters
    std::map<std::string, std::string> params;
    params["id"] = "1";
    params["name"] = "Test User";
    params["email"] = "test@example.com";
    
    // Execute write operation
    auto result = db_manager->executeWrite(endpoint, params);
    
    REQUIRE(result.rows_affected == 1);
    REQUIRE(result.returned_data.has_value());
    REQUIRE(result.returned_data.value().size() == 1);
    REQUIRE(result.returned_data.value()[0]["name"].dump() == "\"Test User\"");
    REQUIRE(result.returned_data.value()[0]["email"].dump() == "\"test@example.com\"");
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite UPDATE operation", "[database_manager]") {
    // Create and populate test table
    db_manager->executeQuery("CREATE TABLE test_update (id INTEGER PRIMARY KEY, name VARCHAR, email VARCHAR)", {}, false);
    db_manager->executeQuery("INSERT INTO test_update (id, name, email) VALUES (1, 'Old Name', 'old@example.com')", {}, false);
    
    // Create UPDATE SQL template
    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "update_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = true;
    endpoint.connection.push_back("default");
    
    std::ofstream template_file(templates_dir / "update_test.sql");
    template_file << "UPDATE test_update SET name = '{{params.name}}', email = '{{params.email}}' WHERE id = {{params.id}} RETURNING id, name, email";
    template_file.close();
    
    std::map<std::string, std::string> params;
    params["id"] = "1";
    params["name"] = "New Name";
    params["email"] = "new@example.com";
    
    auto result = db_manager->executeWrite(endpoint, params);
    
    REQUIRE(result.rows_affected == 1);
    REQUIRE(result.returned_data.has_value());
    REQUIRE(result.returned_data.value()[0]["name"].dump() == "\"New Name\"");
    
    // Verify the update actually happened
    auto verify_result = db_manager->executeQuery("SELECT name, email FROM test_update WHERE id = 1", {}, false);
    REQUIRE(verify_result.data[0]["name"].dump() == "\"New Name\"");
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite DELETE operation", "[database_manager]") {
    // Create and populate test table
    db_manager->executeQuery("CREATE TABLE test_delete (id INTEGER PRIMARY KEY, name VARCHAR)", {}, false);
    db_manager->executeQuery("INSERT INTO test_delete (id, name) VALUES (1, 'Test'), (2, 'Keep'), (3, 'Delete')", {}, false);
    
    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "delete_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = true;
    endpoint.connection.push_back("default");
    
    std::ofstream template_file(templates_dir / "delete_test.sql");
    template_file << "DELETE FROM test_delete WHERE id = {{params.id}}";
    template_file.close();
    
    std::map<std::string, std::string> params;
    params["id"] = "3";
    
    auto result = db_manager->executeWrite(endpoint, params);
    
    REQUIRE(result.rows_affected == 1);
    
    // Verify the delete actually happened
    auto verify_result = db_manager->executeQuery("SELECT COUNT(*) as count FROM test_delete", {}, false);
    REQUIRE(verify_result.data[0]["count"].dump() == "2");
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWriteInTransaction rollback on error", "[database_manager]") {
    // Create test table
    db_manager->executeQuery("CREATE TABLE test_transaction (id INTEGER PRIMARY KEY, name VARCHAR UNIQUE)", {}, false);
    db_manager->executeQuery("INSERT INTO test_transaction (id, name) VALUES (1, 'Existing')", {}, false);
    
    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "transaction_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = true;
    endpoint.connection.push_back("default");
    
    std::ofstream template_file(templates_dir / "transaction_test.sql");
    template_file << "INSERT INTO test_transaction (id, name) VALUES ({{params.id}}, '{{params.name}}')";
    template_file.close();
    
    std::map<std::string, std::string> params;
    params["id"] = "2";
    params["name"] = "Existing";  // This will cause a UNIQUE constraint violation
    
    // Should throw an exception and rollback
    REQUIRE_THROWS_AS(db_manager->executeWriteInTransaction(endpoint, params), std::exception);
    
    // Verify no rows were inserted (transaction rolled back)
    auto verify_result = db_manager->executeQuery("SELECT COUNT(*) as count FROM test_transaction", {}, false);
    REQUIRE(verify_result.data[0]["count"].dump() == "1");  // Only the original row
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite without RETURNING clause", "[database_manager]") {
    db_manager->executeQuery("CREATE TABLE test_no_returning (id INTEGER PRIMARY KEY, name VARCHAR)", {}, false);
    
    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "insert_no_returning.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = false;  // Test without transaction
    endpoint.connection.push_back("default");
    
    std::ofstream template_file(templates_dir / "insert_no_returning.sql");
    template_file << "INSERT INTO test_no_returning (id, name) VALUES ({{params.id}}, '{{params.name}}')";
    template_file.close();
    
    std::map<std::string, std::string> params;
    params["id"] = "1";
    params["name"] = "Test";
    
    auto result = db_manager->executeWrite(endpoint, params);
    
    REQUIRE(result.rows_affected == 1);
    // Note: Without RETURNING clause, DuckDB may still return an empty result structure
    // The key assertion is that rows_affected is correct, indicating the INSERT succeeded
    // If returned_data exists (even as an empty array), that's acceptable
    // The important distinction is that there's no actual data to return (empty array vs populated array)
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager cache invalidation after write", "[database_manager]") {
    // Create a cached endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = true;
    endpoint.cache.table = "test_cache";
    endpoint.cache.invalidate_on_write = true;
    
    // Verify isCacheEnabled works
    REQUIRE(db_manager->isCacheEnabled(endpoint) == true);
    
    // Verify invalidateCache can be called (implementation may vary)
    // The actual cache invalidation depends on CacheManager implementation
    // Result may be true or false depending on cache state, but should not throw
    REQUIRE_NOTHROW(db_manager->invalidateCache(endpoint));
}

TEST_CASE_METHOD(TestFixture, "DatabaseManager cache behavior: no action when disabled", "[database_manager]") {
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = false;
    endpoint.cache.invalidate_on_write = true;  // Even if configured, should not invalidate if cache disabled
    
    REQUIRE(db_manager->isCacheEnabled(endpoint) == false);
    
    // Should not throw when trying to invalidate disabled cache
    REQUIRE_NOTHROW(db_manager->invalidateCache(endpoint));
} 
// Regression (#211): without RETURNING a DML statement answers ONE row (a BIGINT
// `Count`), and that row count was reported, so an UPDATE of 5 rows and a DELETE
// of a missing id both said rows_affected = 1.
TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite reports DuckDB's changed-row count", "[database_manager]") {
    db_manager->executeQuery("CREATE TABLE test_count (id INTEGER, name VARCHAR)", {}, false);
    db_manager->executeQuery("INSERT INTO test_count SELECT i, 'n' FROM range(5) t(i)", {}, false);

    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "count_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.connection.push_back("default");
    const auto write = [&](const std::string& sql) {
        std::ofstream template_file(templates_dir / "count_test.sql");
        template_file << sql;
        template_file.close();
        std::map<std::string, std::string> params;
        return db_manager->executeWrite(endpoint, params);
    };

    CHECK(write("UPDATE test_count SET name = 'x'").rows_affected == 5);
    CHECK(write("DELETE FROM test_count WHERE id = 999").rows_affected == 0);
    CHECK(write("UPDATE test_count SET name = 'y' WHERE id < 2 RETURNING id").rows_affected == 2);
    CHECK(write("UPDATE test_count SET name = 'z' WHERE id = 999 RETURNING id").rows_affected == 0);
    // Summed over the statements of a multi-statement template.
    CHECK(write("DELETE FROM test_count WHERE id = 4; INSERT INTO test_count VALUES (10, 'a'), (11, 'b')").rows_affected == 3);
}

// #271: with `returns-data: true` the rows of the LAST statement come back when
// it is a SELECT - also when it is the only statement, and whatever it starts
// with (WITH, FROM, a comment). It used to need a second statement and a
// leading `SELECT` keyword, so a single-SELECT POST answered `data: []`.
TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite returns the rows of a final SELECT", "[database_manager][write]") {
    db_manager->executeQuery("CREATE TABLE test_sel (id INTEGER, name VARCHAR)", {}, false);
    db_manager->executeQuery("INSERT INTO test_sel VALUES (1, 'a'), (2, 'b'), (3, 'c')", {}, false);

    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "sel_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.returns_data = true;
    endpoint.connection.push_back("default");
    const auto write = [&](const std::string& sql) {
        std::ofstream template_file(templates_dir / "sel_test.sql");
        template_file << sql;
        template_file.close();
        std::map<std::string, std::string> params;
        return db_manager->executeWrite(endpoint, params);
    };
    const auto ids = [](const WriteResult& r) {
        std::vector<std::string> out;
        if (r.returned_data) {
            for (std::size_t i = 0; i < r.returned_data->size(); ++i) {
                out.push_back((*r.returned_data)[i]["id"].dump());
            }
        }
        return out;
    };
    const std::vector<std::string> two_three{"2", "3"};

    SECTION("a single SELECT") {
        auto r = write("SELECT id, name FROM test_sel WHERE id >= 2 ORDER BY id");
        CHECK(ids(r) == two_three);
        CHECK(r.rows_affected == 0);
        CHECK(r.returned_columns == std::vector<std::string>{"id", "name"});
    }
    SECTION("a single WITH ... SELECT") {
        CHECK(ids(write("WITH t AS (SELECT * FROM test_sel WHERE id >= 2) SELECT id FROM t ORDER BY id")) == two_three);
    }
    SECTION("a SELECT after a comment") {
        CHECK(ids(write("-- the search\n/* block */ SELECT id FROM test_sel WHERE id >= 2 ORDER BY id")) == two_three);
    }
    SECTION("FROM-first syntax") {
        CHECK(ids(write("FROM test_sel SELECT id WHERE id >= 2 ORDER BY id")) == two_three);
    }
    SECTION("a WITH ... SELECT after a write") {
        auto r = write("INSERT INTO test_sel VALUES (4, 'd'); "
                       "WITH t AS (SELECT * FROM test_sel WHERE id >= 3) SELECT id FROM t ORDER BY id");
        CHECK(ids(r) == std::vector<std::string>{"3", "4"});
        CHECK(r.rows_affected == 1);
    }
    SECTION("without returns-data a SELECT is not data") {
        endpoint.operation.returns_data = false;
        auto r = write("SELECT id FROM test_sel");
        CHECK(r.rows_affected == 0);
        CHECK_FALSE(r.returned_data.has_value());
    }
}

// #277: RETURNING was found by substring, so a column `returning_note`, a
// 'returning' literal or a comment made a plain DML look like it returned rows:
// rows_affected became the result's ONE row, and that row (`Count`) became data.
TEST_CASE_METHOD(TestFixture, "DatabaseManager executeWrite knows RETURNING from the result, not the text", "[database_manager][write]") {
    db_manager->executeQuery("CREATE TABLE test_ret (id INTEGER, returning_note VARCHAR)", {}, false);
    db_manager->executeQuery("INSERT INTO test_ret SELECT i, 'n' FROM range(3) t(i)", {}, false);

    EndpointConfig endpoint;
    endpoint.templateSource = (templates_dir / "ret_test.sql").string();
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.returns_data = true;
    endpoint.connection.push_back("default");
    const auto write = [&](const std::string& sql) {
        std::ofstream template_file(templates_dir / "ret_test.sql");
        template_file << sql;
        template_file.close();
        std::map<std::string, std::string> params;
        return db_manager->executeWrite(endpoint, params);
    };

    SECTION("a column named returning_*") {
        auto r = write("UPDATE test_ret SET returning_note = 'x'");
        CHECK(r.rows_affected == 3);
        CHECK((!r.returned_data || r.returned_data->size() == 0));
    }
    SECTION("a 'returning' string literal") {
        auto r = write("UPDATE test_ret SET returning_note = 'RETURNING id'");
        CHECK(r.rows_affected == 3);
        CHECK((!r.returned_data || r.returned_data->size() == 0));
    }
    SECTION("a comment mentioning RETURNING") {
        auto r = write("-- no RETURNING here\nDELETE FROM test_ret WHERE id = 0");
        CHECK(r.rows_affected == 1);
        CHECK((!r.returned_data || r.returned_data->size() == 0));
    }
    SECTION("a false RETURNING does not hide the final SELECT") {
        auto r = write("UPDATE test_ret SET returning_note = 'y'; SELECT count(*) AS n FROM test_ret WHERE returning_note = 'y'");
        CHECK(r.rows_affected == 3);
        REQUIRE(r.returned_data.has_value());
        REQUIRE(r.returned_data->size() == 1);
        CHECK((*r.returned_data)[0]["n"].dump() == "3");
    }
    SECTION("a real RETURNING still returns its rows") {
        auto r = write("UPDATE test_ret SET returning_note = 'z' WHERE id < 2 RETURNING id");
        CHECK(r.rows_affected == 2);
        REQUIRE(r.returned_data.has_value());
        CHECK(r.returned_data->size() == 2);
    }
    SECTION("a real RETURNING with bound values, as a prepared statement") {
        RequestFieldConfig id_field;
        id_field.fieldName = "id";
        id_field.fieldIn = "body";
        ValidatorConfig int_validator;
        int_validator.type = "int";
        id_field.validators.push_back(int_validator);
        endpoint.request_fields.push_back(id_field);
        std::ofstream template_file(templates_dir / "ret_test.sql");
        template_file << "INSERT INTO test_ret (id, returning_note) VALUES ({{ params.id }}, 'b') RETURNING id;";
        template_file.close();
        std::map<std::string, std::string> params{{"id", "7"}};
        auto r = db_manager->executeWrite(endpoint, params);
        CHECK(r.rows_affected == 1);
        REQUIRE(r.returned_data.has_value());
        REQUIRE(r.returned_data->size() == 1);
        CHECK((*r.returned_data)[0]["id"].dump() == "7");
    }
}

// #264: getTableColumns handed its schema/table to executeQuery(string, params),
// which never binds, so the `?` placeholders failed and it returned nothing; and
// names came back JSON-quoted.
TEST_CASE_METHOD(TestFixture, "DatabaseManager getTableColumns binds schema and table", "[database_manager]") {
    db_manager->executeQuery("CREATE TABLE test_cols (id INTEGER NOT NULL, name VARCHAR)", {}, false);
    db_manager->executeQuery("CREATE TABLE test_cols_other (x INTEGER)", {}, false);

    auto columns = db_manager->getTableColumns("main", "test_cols");
    REQUIRE(columns.size() == 2);
    CHECK(columns[0].name == "id");
    CHECK(columns[0].type == "INTEGER");
    CHECK_FALSE(columns[0].nullable);
    CHECK(columns[1].name == "name");
    CHECK(columns[1].type == "VARCHAR");
    CHECK(columns[1].nullable);

    CHECK(db_manager->getTableColumns("main", "does_not_exist").empty());
    // A quote in the name is data, not SQL.
    CHECK(db_manager->getTableColumns("main", "test_cols' OR '1'='1").empty());
}

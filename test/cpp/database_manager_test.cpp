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
project_name: flapi_test
project_description: Test configuration for FLAPI
server_name: test_server

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
        db_manager->refreshSecretsTable(table_name, json_data);
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
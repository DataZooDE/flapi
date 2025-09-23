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

database:
  path: )" << db_path.string() << R"(
  settings:
    memory_limit: 2GB
    threads: 4

template:
  path: )" << templates_dir.string() << R"(

endpoints:
  test_endpoint:
    method: GET
    path: /test
    templateSource: SELECT 1 as value
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
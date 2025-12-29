#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "config_service.hpp"
#include "config_manager.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace flapi;
namespace fs = std::filesystem;

namespace {

// Helper to create a test filesystem structure
class FilesystemTestFixture {
public:
    fs::path temp_dir;
    fs::path templates_dir;
    fs::path config_path;
    
    FilesystemTestFixture() {
        temp_dir = fs::temp_directory_path() / "flapi_filesystem_tests";
        fs::create_directories(temp_dir);
        templates_dir = temp_dir / "templates";
        fs::create_directories(templates_dir);
        
        // Create flapi.yaml at the base
        config_path = temp_dir / "flapi.yaml";
        std::ofstream config_file(config_path);
        config_file << "project-name: test\nproject-description: Test Filesystem\ntemplate:\n  path: " 
                    << templates_dir.string() << "\n";
        config_file.close();
    }
    
    ~FilesystemTestFixture() {
        fs::remove_all(temp_dir);
    }
    
    void createEndpoint(const std::string& name, const std::string& url_path, 
                       bool with_cache = false, const std::string& subdir = "") {
        fs::path target_dir = subdir.empty() ? templates_dir : templates_dir / subdir;
        fs::create_directories(target_dir);
        
        // Create YAML file
        auto yaml_path = target_dir / (name + ".yaml");
        std::ofstream yaml_file(yaml_path);
        yaml_file << "url-path: " << url_path << "\n";
        yaml_file << "template-source: " << name << ".sql\n";
        yaml_file << "connection:\n  - default\n";
        if (with_cache) {
            yaml_file << "cache:\n  enabled: true\n  table: " << name << "_cache\n";
            yaml_file << "  schema: cache\n  template-file: " << name << "_cache.sql\n";
        }
        yaml_file.close();
        
        // Create SQL template file
        auto sql_path = target_dir / (name + ".sql");
        std::ofstream sql_file(sql_path);
        sql_file << "SELECT 'Hello from " << name << "' AS message;\n";
        sql_file.close();
        
        // Create cache SQL if needed
        if (with_cache) {
            auto cache_sql_path = target_dir / (name + "_cache.sql");
            std::ofstream cache_sql_file(cache_sql_path);
            cache_sql_file << "SELECT * FROM source WHERE updated_at > '{{cache.snapshotTimestamp}}';\n";
            cache_sql_file.close();
        }
    }
    
    void createMcpTool(const std::string& name, const std::string& tool_name, 
                      const std::string& subdir = "") {
        fs::path target_dir = subdir.empty() ? templates_dir : templates_dir / subdir;
        fs::create_directories(target_dir);
        
        auto yaml_path = target_dir / (name + ".yaml");
        std::ofstream yaml_file(yaml_path);
        yaml_file << "mcp-tool:\n  name: " << tool_name << "\n";
        yaml_file << "  description: Test MCP tool\n";
        yaml_file << "template-source: " << name << ".sql\n";
        yaml_file << "connection:\n  - default\n";
        yaml_file.close();
        
        auto sql_path = target_dir / (name + ".sql");
        std::ofstream sql_file(sql_path);
        sql_file << "SELECT * FROM data;\n";
        sql_file.close();
    }
    
    void createSharedYaml(const std::string& name, const std::string& subdir = "") {
        fs::path target_dir = subdir.empty() ? templates_dir : templates_dir / subdir;
        fs::create_directories(target_dir);
        
        auto yaml_path = target_dir / (name + ".yaml");
        std::ofstream yaml_file(yaml_path);
        yaml_file << "# Shared configuration\n";
        yaml_file << "auth:\n  enabled: false\n";
        yaml_file.close();
    }
    
    void createDirectory(const std::string& subdir) {
        fs::create_directories(templates_dir / subdir);
    }
    
    void createStandaloneFile(const std::string& filename, const std::string& content, 
                             const std::string& subdir = "") {
        fs::path target_dir = subdir.empty() ? templates_dir : templates_dir / subdir;
        fs::create_directories(target_dir);
        
        auto file_path = target_dir / filename;
        std::ofstream file(file_path);
        file << content;
        file.close();
    }
};

} // anonymous namespace

TEST_CASE("ConfigService: getFilesystemStructure - basic structure", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    // Create a simple structure
    fixture.createEndpoint("users", "/users", false);
    fixture.createEndpoint("products", "/products", true);
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Check basic structure
    REQUIRE(json.has("config_file_exists"));
    REQUIRE(json["config_file_exists"].b() == true);
    REQUIRE(json["config_file"].s() == std::string("flapi.yaml"));
    REQUIRE(json.has("tree"));
    REQUIRE(json["tree"].size() >= 2);
    
    // Find users.yaml in the tree
    bool found_users = false;
    bool found_products = false;
    
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["name"].s() == std::string("users.yaml")) {
            found_users = true;
            REQUIRE(node["type"].s() == std::string("file"));
            REQUIRE(node["yaml_type"].s() == std::string("endpoint"));
            REQUIRE(node["url_path"].s() == std::string("/users"));
            REQUIRE(node["template_source"].s() == std::string("users.sql"));
        }
        if (node["name"].s() == std::string("products.yaml")) {
            found_products = true;
            REQUIRE(node["yaml_type"].s() == std::string("endpoint"));
            REQUIRE(node.has("cache_template_source"));
        }
    }
    
    REQUIRE(found_users);
    REQUIRE(found_products);
}

TEST_CASE("ConfigService: getFilesystemStructure - nested directories", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    // Create nested structure
    fixture.createDirectory("api");
    fixture.createDirectory("api/v1");
    fixture.createEndpoint("users", "/api/v1/users", false, "api/v1");
    fixture.createEndpoint("orders", "/api/v1/orders", true, "api/v1");
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Find the api directory
    bool found_api_dir = false;
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["name"].s() == std::string("api") && node["type"].s() == std::string("directory")) {
            found_api_dir = true;
            REQUIRE(node.has("children"));
            
            // Check for v1 subdirectory
            bool found_v1 = false;
            for (size_t j = 0; j < node["children"].size(); j++) {
                auto& child = node["children"][j];
                if (child["name"].s() == std::string("v1") && child["type"].s() == std::string("directory")) {
                    found_v1 = true;
                    REQUIRE(child.has("children"));
                    
                    // Check for endpoint files
                    bool found_users_yaml = false;
                    bool found_users_sql = false;
                    
                    for (size_t k = 0; k < child["children"].size(); k++) {
                        auto& file = child["children"][k];
                        if (file["name"].s() == std::string("users.yaml")) {
                            found_users_yaml = true;
                            REQUIRE(file["yaml_type"].s() == std::string("endpoint"));
                        }
                        if (file["name"].s() == std::string("users.sql")) {
                            found_users_sql = true;
                        }
                    }
                    
                    REQUIRE(found_users_yaml);
                    REQUIRE(found_users_sql);
                }
            }
            REQUIRE(found_v1);
        }
    }
    
    REQUIRE(found_api_dir);
}

TEST_CASE("ConfigService: getFilesystemStructure - MCP tools", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    fixture.createMcpTool("get_users", "get_users");
    fixture.createMcpTool("search_products", "search_products", "mcp");
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Find MCP tool in root
    bool found_mcp_tool = false;
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["name"].s() == std::string("get_users.yaml")) {
            found_mcp_tool = true;
            REQUIRE(node["yaml_type"].s() == std::string("mcp-tool"));
            REQUIRE(node["mcp_name"].s() == std::string("get_users"));
        }
    }
    
    REQUIRE(found_mcp_tool);
}

TEST_CASE("ConfigService: getFilesystemStructure - shared configs", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    fixture.createSharedYaml("common-auth");
    fixture.createSharedYaml("common-rate-limit", "shared");
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Find shared YAML in root
    bool found_shared = false;
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["name"].s() == std::string("common-auth.yaml")) {
            found_shared = true;
            REQUIRE(node["yaml_type"].s() == std::string("shared"));
        }
    }
    
    REQUIRE(found_shared);
}

TEST_CASE("ConfigService: getFilesystemStructure - mixed file types", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    fixture.createEndpoint("users", "/users", false);
    fixture.createMcpTool("get_data", "get_data");
    fixture.createSharedYaml("common");
    fixture.createStandaloneFile("standalone.sql", "SELECT 1;");
    fixture.createStandaloneFile("README.md", "# Documentation");
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Count different file types
    int endpoints = 0;
    int mcp_tools = 0;
    int shared_yamls = 0;
    int sql_files = 0;
    int other_files = 0;
    
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["type"].s() != std::string("directory")) {
            if (node.has("yaml_type")) {
                auto yaml_type = node["yaml_type"].s();
                if (yaml_type == std::string("endpoint")) endpoints++;
                else if (yaml_type == std::string("mcp-tool")) mcp_tools++;
                else if (yaml_type == std::string("shared")) shared_yamls++;
            } else {
                auto ext = node["extension"].s();
                if (ext == std::string(".sql")) sql_files++;
                else other_files++;
            }
        }
    }
    
    REQUIRE(endpoints == 1);
    REQUIRE(mcp_tools == 1);
    REQUIRE(shared_yamls == 1);
    REQUIRE(sql_files >= 1); // At least the standalone.sql
    REQUIRE(other_files >= 1); // At least README.md
}

TEST_CASE("ConfigService: getFilesystemStructure - empty directory", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    // Don't create any files, just the directory structure
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json.has("tree"));
    REQUIRE(json["tree"].size() == 0); // Empty tree
}

TEST_CASE("ConfigService: getFilesystemStructure - sorting", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    // Create files in non-alphabetical order
    fixture.createEndpoint("zebra", "/zebra", false);
    fixture.createDirectory("apple");
    fixture.createEndpoint("banana", "/banana", false, "apple");
    fixture.createDirectory("cherry");
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Check that directories come before files
    bool found_first_dir = false;
    bool found_first_file = false;
    
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["type"].s() == std::string("directory") && !found_first_dir) {
            found_first_dir = true;
        } else if (node["type"].s() == std::string("file") && !found_first_file) {
            found_first_file = true;
            // If we found a file, we should have already seen all directories
            REQUIRE(found_first_dir);
        }
    }
}

TEST_CASE("ConfigService: getFilesystemStructure - file relationships", "[config_service][filesystem]") {
    FilesystemTestFixture fixture;
    
    // Create endpoint with cache
    fixture.createEndpoint("products", "/products", true);
    
    auto config_mgr = std::make_shared<ConfigManager>(fixture.config_path);
    config_mgr->loadConfig();
    
    ConfigService service(config_mgr);
    
    crow::request req;
    auto response = service.getFilesystemStructure(req);
    
    REQUIRE(response.code == crow::status::OK);
    
    auto json = crow::json::load(response.body);
    
    // Find the products.yaml and verify relationships
    for (size_t i = 0; i < json["tree"].size(); i++) {
        auto& node = json["tree"][i];
        if (node["name"].s() == std::string("products.yaml")) {
            REQUIRE(node.has("template_source"));
            REQUIRE(node["template_source"].s() == std::string("products.sql"));
            REQUIRE(node.has("cache_template_source"));
            REQUIRE(node["cache_template_source"].s() == std::string("products_cache.sql"));
        }
    }
}


#include <argparse/argparse.hpp>
#include <exception>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

#include "api_server.hpp"
#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "rate_limit_middleware.hpp"
#include "config_token_utils.hpp"
#include "credential_manager.hpp"
#include "vfs_health_checker.hpp"

using namespace flapi;

// Add global variable for signal handling
std::atomic<bool> should_exit(false);
std::shared_ptr<APIServer> api_server;

void set_log_level(const std::string& log_level) {
    if (log_level == "debug") {
        crow::logger::setLogLevel(crow::LogLevel::Debug);
    } else if (log_level == "info") {
        crow::logger::setLogLevel(crow::LogLevel::Info);
    } else if (log_level == "warning") {
        crow::logger::setLogLevel(crow::LogLevel::Warning);
    } else if (log_level == "error") {
        crow::logger::setLogLevel(crow::LogLevel::Error);
    } else {
        std::cerr << "Invalid log level: " << log_level << ". Using default (info)." << std::endl;
        crow::logger::setLogLevel(crow::LogLevel::Info);
    }
}

std::shared_ptr<ConfigManager> initializeConfig(const std::string& config_file) {
    std::shared_ptr<ConfigManager> config_manager = std::make_shared<ConfigManager>(std::filesystem::path(config_file));
    try {
        config_manager->loadConfig();
    } catch (const std::exception& e) {
        throw std::runtime_error("Error while loading configuration, Details: " + std::string(e.what()));
    }
    return config_manager;
}

std::string getEndpointName(const EndpointConfig& endpoint) {
    if (!endpoint.urlPath.empty()) {
        return endpoint.urlPath;
    }
    if (endpoint.mcp_tool) {
        return endpoint.mcp_tool->name;
    }
    if (endpoint.mcp_resource) {
        return endpoint.mcp_resource->name;
    }
    if (endpoint.mcp_prompt) {
        return endpoint.mcp_prompt->name;
    }
    return "unknown";
}

void printValidationErrors(const std::string& endpoint_name, const std::vector<std::string>& errors, int& errors_count) {
    std::cerr << "\n✗ Endpoint: " << endpoint_name << std::endl;
    for (const auto& error : errors) {
        std::cerr << "  ERROR: " << error << std::endl;
        errors_count++;
    }
}

void printValidationWarnings(const std::string& endpoint_name, const std::vector<std::string>& warnings, int& warnings_count) {
    std::cout << "\n⚠ Endpoint: " << endpoint_name << std::endl;
    for (const auto& warning : warnings) {
        std::cout << "  WARNING: " << warning << std::endl;
        warnings_count++;
    }
}

void printValidationSummary(bool all_valid, int errors_count, int warnings_count) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    if (all_valid) {
        std::cout << "✓ Validation PASSED" << std::endl;
        if (warnings_count > 0) {
            std::cout << "  " << warnings_count << " warning(s)" << std::endl;
        }
    } else {
        std::cerr << "✗ Validation FAILED" << std::endl;
        std::cerr << "  " << errors_count << " error(s)" << std::endl;
        if (warnings_count > 0) {
            std::cerr << "  " << warnings_count << " warning(s)" << std::endl;
        }
    }
}

int validateConfiguration(std::shared_ptr<ConfigManager> config_manager, const std::string& config_file) {
    std::cout << "Validating configuration file: " << config_file << std::endl;
    std::cout << "✓ Configuration file loaded successfully" << std::endl;
    std::cout << "✓ Parsed " << config_manager->getEndpoints().size() << " endpoint(s)" << std::endl;
    
    bool all_valid = true;
    int warnings_count = 0;
    int errors_count = 0;
    
    for (const auto& endpoint : config_manager->getEndpoints()) {
        auto result = config_manager->validateEndpointConfig(endpoint);
        std::string endpoint_name = getEndpointName(endpoint);
        
        if (!result.valid) {
            all_valid = false;
            printValidationErrors(endpoint_name, result.errors, errors_count);
        }
        
        if (!result.warnings.empty()) {
            printValidationWarnings(endpoint_name, result.warnings, warnings_count);
        }
    }
    
    printValidationSummary(all_valid, errors_count, warnings_count);
    return all_valid ? 0 : 1;
}

void initializeDatabase(std::shared_ptr<ConfigManager> config_manager) {
    try {
        DatabaseManager::getInstance()->initializeDBManagerFromConfig(config_manager);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error creating database, Details: " + std::string(e.what()));
    }
}

void initializeCloudCredentials() {
    CROW_LOG_INFO << "Initializing cloud storage credentials...";
    auto& cred_manager = flapi::getGlobalCredentialManager();
    cred_manager.loadFromEnvironment();
    cred_manager.logCredentialStatus();
}

void configureCloudCredentialsInDuckDB() {
    auto& cred_manager = flapi::getGlobalCredentialManager();
    if (cred_manager.hasS3Credentials() || cred_manager.hasGCSCredentials() || cred_manager.hasAzureCredentials()) {
        CROW_LOG_INFO << "Configuring cloud credentials in DuckDB...";
        if (cred_manager.configureDuckDB()) {
            CROW_LOG_INFO << "Cloud credentials configured successfully";
        } else {
            CROW_LOG_WARNING << "Failed to configure some cloud credentials in DuckDB";
        }
    }
}

void verifyStorageHealth(std::shared_ptr<ConfigManager> config_manager) {
    flapi::VFSHealthChecker health_checker;
    std::string config_path = config_manager->getBasePath();
    std::string templates_path = config_manager->getTemplatePath();
    health_checker.verifyStartupHealth(config_path, templates_path);
}

void terminateHandler() {
    CROW_LOG_ERROR << "Unhandled exception caught! flapi is giving up :-(";

    auto ex = std::current_exception();
    try {
        std::rethrow_exception (ex);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "exception caught: " << e.what();
    }
    std::abort();
}

#ifdef _WIN32

void writeMiniDump(EXCEPTION_POINTERS* exceptionInfo, const std::string& filename) {
    HANDLE hFile = CreateFile(filename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!hFile || hFile == INVALID_HANDLE_VALUE) {
        CROW_LOG_ERROR << "Failed to create dump file " << filename;
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = exceptionInfo;
    mdei.ClientPointers = FALSE;

    MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpNormal,
        &mdei,
        NULL,
        NULL
    );
    CloseHandle(hFile);
}

LONG WINAPI windowsExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    CROW_LOG_ERROR << "Unhandled Windows exception caught!";
    CROW_LOG_ERROR << "Exception code: " << std::hex << exceptionInfo->ExceptionRecord->ExceptionCode;
    CROW_LOG_ERROR << "Address: " << exceptionInfo->ExceptionRecord->ExceptionAddress;

    std::string filename = "crash_dump_" + std::to_string(GetCurrentProcessId()) + ".dmp";
    CROW_LOG_ERROR << "Writing crash dump to " << filename;
    
    writeMiniDump(exceptionInfo, filename);

    return EXCEPTION_EXECUTE_HANDLER;
}

#endif


void signal_handler(int signal) {
    if (signal == SIGINT) {
        CROW_LOG_INFO << "Received SIGINT, shutting down...";
        should_exit = true;
        if (api_server) {
            api_server->stop();
        }
    }
}

int main(int argc, char* argv[]) 
{
    std::set_terminate(terminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(windowsExceptionHandler);
    signal(SIGINT, signal_handler);
#else
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
#endif

    static argparse::ArgumentParser program("flapi");

    program.add_argument("-c", "--config")
        .help("Path to the flapi.yaml configuration file")
        .default_value(std::string("flapi.yaml"));

    program.add_argument("-p", "--port")
        .help("Port number for the web server")
        .default_value(-1)
        .scan<'i', int>();

    program.add_argument("--log-level")
        .help("Set the log level (debug, info, warning, error)")
        .default_value(std::string("info"));

    program.add_argument("--validate-config")
        .help("Validate the configuration file and exit")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--config-service")
        .help("Enable the configuration service API")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--config-service-token")
        .help("Authentication token for configuration service API")
        .default_value(std::string(""));

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    std::string config_file = program.get<std::string>("--config");
    int cmd_port = program.get<int>("--port");
    std::string log_level = program.get<std::string>("--log-level");
    bool validate_config = program.get<bool>("--validate-config");
    bool config_service_enabled = program.get<bool>("--config-service");
    std::string config_service_token = program.get<std::string>("--config-service-token");

    // Check environment variable for config service token if not provided via CLI
    if (config_service_token.empty()) {
        const char* env_token = std::getenv("FLAPI_CONFIG_SERVICE_TOKEN");
        if (env_token != nullptr) {
            config_service_token = env_token;
        }
    }

    // If config service is enabled but no token provided, generate one
    if (config_service_enabled && config_service_token.empty()) {
        config_service_token = ConfigTokenUtils::generateSecureToken();
        CROW_LOG_INFO << "Generated config service token (no token was provided)";
    }

    set_log_level(log_level);

    auto config_manager = initializeConfig(config_file);

    // If validate-config flag is set, validate and exit
    if (validate_config) {
        return validateConfiguration(config_manager, config_file);
    }

    // Initialize cloud storage credentials (reads environment variables)
    initializeCloudCredentials();

    if (cmd_port != -1) {
        config_manager->setHttpPort(cmd_port);
    }

    initializeDatabase(config_manager);

    // Configure cloud credentials in DuckDB after database is initialized
    configureCloudCredentialsInDuckDB();

    // Verify storage health at startup
    verifyStorageHealth(config_manager);

    // Create unified API server with MCP support (always enabled in unified configuration)
    api_server = std::make_shared<APIServer>(
        config_manager, 
        DatabaseManager::getInstance(), 
        config_service_enabled,
        config_service_token
    );

    // Start unified server
    std::thread unified_server_thread([config_manager, server = api_server]() {
        server->run(config_manager->getHttpPort());
    });

    CROW_LOG_INFO << "flAPI unified server started - REST API and MCP on port " << config_manager->getHttpPort();
    
    // Print config service token prominently if enabled
    if (config_service_enabled) {
        std::cout << "\n";
        std::cout << "============================================================\n";
        std::cout << "    CONFIG SERVICE ENABLED\n";
        std::cout << "============================================================\n";
        std::cout << "    Token: " << config_service_token << "\n";
        std::cout << "============================================================\n";
        std::cout << "\n";
        std::cout << "Use this token to authenticate configuration API requests:\n";
        std::cout << "  Authorization: Bearer " << config_service_token << "\n";
        std::cout << "or\n";
        std::cout << "  X-Config-Token: " << config_service_token << "\n";
        std::cout << "\n";
    }

    // Wait for server to finish
    unified_server_thread.join();

    return 0;
}

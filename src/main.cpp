#include <argparse/argparse.hpp>
#include <exception>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

#include "api_server.hpp"
#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "rate_limit_middleware.hpp"

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

void initializeDatabase(std::shared_ptr<ConfigManager> config_manager) {
    try {
        DatabaseManager::getInstance()->initializeDBManagerFromConfig(config_manager);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error creating database, Details: " + std::string(e.what()));
    }
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

    program.add_argument("--enable-config-ui")
        .help("Enable the configuration UI")
        .default_value(false);


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

    set_log_level(log_level);

    auto config_manager = initializeConfig(config_file);

    if (cmd_port != -1) {
        config_manager->setHttpPort(cmd_port);
    }

    initializeDatabase(config_manager);

    bool enable_config_ui = program.get<bool>("--enable-config-ui");

    // Create unified API server with MCP support (always enabled in unified configuration)
    api_server = std::make_shared<APIServer>(config_manager, DatabaseManager::getInstance(), enable_config_ui);

    // Start unified server
    std::thread unified_server_thread([config_manager, server = api_server]() {
        server->run(config_manager->getHttpPort());
    });

    CROW_LOG_INFO << "flAPI unified server started - REST API and MCP on port " << config_manager->getHttpPort();

    // Wait for server to finish
    unified_server_thread.join();

    return 0;
}

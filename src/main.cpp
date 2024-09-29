#include "api_server.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "rate_limit_middleware.hpp"
#include "auth_middleware.hpp"
#include <argparse/argparse.hpp>
#include <iostream>

using namespace flapi;

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
    std::shared_ptr<ConfigManager> config_manager = std::make_shared<ConfigManager>();
    try {
        config_manager->loadConfig(config_file);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error loading configuration file: " + config_file + ", Error: " + std::string(e.what()));
    }
    return config_manager;
}

void initializeDatabase(std::shared_ptr<ConfigManager> config_manager) {
    try {
        DatabaseManager::getInstance()->initializeDBManagerFromConfig(config_manager);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error creating database: " + std::string(e.what()));
    }
}

int main(int argc, char* argv[]) 
{
    static argparse::ArgumentParser program("flapi");

    program.add_argument("-c", "--config")
        .help("Path to the flapi.yaml configuration file")
        .default_value(std::string("flapi.yaml"));

    program.add_argument("-p", "--port")
        .help("Port number for the web server")
        .default_value(8080)
        .scan<'i', int>();

    program.add_argument("--log-level")
        .help("Set the log level (debug, info, warning, error)")
        .default_value(std::string("info"));

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    std::string config_file = program.get<std::string>("--config");
    int port = program.get<int>("--port");
    std::string log_level = program.get<std::string>("--log-level");

    set_log_level(log_level);

    auto config_manager = initializeConfig(config_file);
    initializeDatabase(config_manager);

    APIServer server(config_manager, DatabaseManager::getInstance());
    server.run(port);

    return 0;
}
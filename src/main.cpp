#include <argparse/argparse.hpp>
#include <exception>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
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
#include "archive_io.hpp"
#include "auth_middleware.hpp"
#include "bundle_locator.hpp"
#include "config_manager.hpp"
#include "flapi_log_handler.hpp"
#include "flapi_tracing.hpp"
#include "database_manager.hpp"
#include "duckdb_embed_fs.hpp"
#include "pack.hpp"
#include "flapi_telemetry.hpp"
#include "rate_limit_middleware.hpp"
#include "config_token_utils.hpp"
#include "credential_manager.hpp"
#include "security_auditor.hpp"
#include "selfpath.hpp"
#include "vfs_adapter.hpp"
#include "vfs_health_checker.hpp"
#include "datazoo_banner.hpp"

using namespace flapi;

// Add global variable for signal handling
std::atomic<bool> should_exit(false);

// Written by main, read by the shutdown supervisor thread. A std::shared_ptr
// is not safe for concurrent read/write, and the supervisor now starts before
// main builds the server - so both sides go through this mutex.
static std::mutex api_server_mutex;
std::shared_ptr<APIServer> api_server;

static void setApiServer(std::shared_ptr<APIServer> server) {
    std::lock_guard<std::mutex> lock(api_server_mutex);
    api_server = std::move(server);
}

static std::shared_ptr<APIServer> getApiServer() {
    std::lock_guard<std::mutex> lock(api_server_mutex);
    return api_server;
}

// Derive a single, bounded auth_kind for the server_started envelope.
// flapi auth is per-endpoint with an optional global block and a separate MCP
// block; we collapse that to one enum {none,basic,bearer,oidc} for telemetry.
// Counts/kinds only — never routes, users, secrets.
static std::string deriveAuthKind(const ConfigManager& cfg) {
    auto normalize = [](const std::string& t) -> std::string {
        if (t == "basic" || t == "bearer" || t == "oidc") {
            return t;
        }
        return "";
    };

    if (cfg.isAuthEnabled()) {
        std::string t = normalize(cfg.getGlobalAuthConfig().type);
        if (!t.empty()) {
            return t;
        }
    }
    for (const auto& endpoint : *cfg.getEndpoints()) {
        if (endpoint.auth.enabled) {
            std::string t = normalize(endpoint.auth.type);
            if (!t.empty()) {
                return t;
            }
        }
    }
    const auto& mcp_auth = cfg.getMCPConfig().auth;
    if (mcp_auth.enabled) {
        std::string t = normalize(mcp_auth.type);
        if (!t.empty()) {
            return t;
        }
    }
    return "none";
}

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

// Detects whether the running binary has a ZIP bundle appended to it and,
// if so, decompresses it once and hands it to FileProviderFactory so all
// later config / SQL-template reads come from the in-memory map.
// Failure to read or decompress is logged at WARNING and silently falls
// back to filesystem mode -- the spike's safety-net behaviour (#42).
void detectAndRegisterEmbeddedBundle() {
    auto loc = LocateBundleInSelf();
    if (!loc.has_value()) {
        return;
    }

    try {
        const auto self_path = GetSelfPath();
        std::ifstream f(self_path, std::ios::binary);
        if (!f.is_open()) {
            CROW_LOG_WARNING << "Bundle detected but self-binary unreadable; "
                                "falling back to filesystem mode";
            return;
        }

        std::vector<std::uint8_t> bytes(loc->size);
        f.seekg(static_cast<std::streamoff>(loc->offset), std::ios::beg);
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(loc->size));
        if (!f) {
            CROW_LOG_WARNING << "Bundle read truncated; falling back to filesystem mode";
            return;
        }

        auto entries = std::make_shared<ArchiveEntries>(ReadArchive(bytes));
        const std::size_t entry_count = entries->size();
        FileProviderFactory::SetBundleContents(std::move(entries));
        CROW_LOG_INFO << "Bundle detected and registered (" << entry_count << " entries)";
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Bundle detection failed (" << e.what()
                         << "); falling back to filesystem mode";
    }
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

void printSecurityWarnings(const std::vector<SecurityWarning>& warnings) {
    if (warnings.empty()) {
        return;
    }
    std::cerr << "\n" << std::string(60, '=') << std::endl;
    std::cerr << "SECURITY WARNINGS (" << warnings.size() << ")" << std::endl;
    std::cerr << std::string(60, '=') << std::endl;
    for (const auto& w : warnings) {
        std::cerr << "[" << w.code << "] " << w.message;
        if (!w.location.empty()) {
            std::cerr << " (at: " << w.location << ")";
        }
        std::cerr << std::endl;
    }
    std::cerr << std::endl;
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
    std::cout << "✓ Parsed " << config_manager->getEndpoints()->size() << " endpoint(s)" << std::endl;
    
    bool all_valid = true;
    int warnings_count = 0;
    int errors_count = 0;
    
    for (const auto& endpoint : *config_manager->getEndpoints()) {
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


// The work a SIGTERM has to cause. NOT run in the signal handler - see below.
static void performShutdown(int signal_number) {
    CROW_LOG_INFO << "Received " << (signal_number == SIGINT ? "SIGINT" : "SIGTERM")
                  << ", shutting down...";
    // May run before the server exists: the supervisor starts as soon as the
    // handlers are installed, so that a SIGTERM arriving during config load
    // or DB init is acted on rather than dropped. Everything below is
    // null-tolerant; api_server is checked at the end.
    // Drain buffered telemetry before exit: the library's at-exit handler
    // discards in-flight events by design, so a server must flush explicitly.
    // Flush spans before the process dies. On a platform with a short
    // SIGTERM grace (Cloud Run, App Runner) this is the difference between
    // having the trace of the request that killed you and not.
    // flush.timeout_ms, not a hardcoded 2s: an operator who tunes the flush
    // budget expects it to apply to the path that matters most here.
    flapi::Tracing().forceFlush(flapi::Tracing().shutdownFlushBudget());
    flapi::GlobalTelemetry().flush();
    if (auto server = getApiServer()) {
        server->stop();
    }
}

#ifndef _WIN32
// Self-pipe. The only thing a signal handler may touch here besides an
// atomic: write(2) is async-signal-safe, everything performShutdown does is
// not.
//
// It used to run performShutdown's work directly in the handler: logging,
// two flushes that take locks and do I/O, and APIServer::stop(), which now
// calls HandlerPool::shutdown() - a mutex plus a join of every worker. A
// signal is delivered on whichever thread happens to be running, so SIGTERM
// landing on a pool worker meant that worker joining ITSELF, and a signal
// arriving while any thread held the pool mutex meant re-entering it. Either
// hangs the process until the platform SIGKILLs it, mid-write.
static int g_shutdown_pipe_read = -1;

// Atomic, and set to -1 BEFORE the close. The handler tests it before
// writing, so a plain int left a window in which a signal could write into an
// fd number that teardown had already closed and something else had reopened.
static std::atomic<int> g_shutdown_pipe_write{-1};

// Creates the self-pipe with both ends close-on-exec.
//
// `pipe2` is a Linux extension. macOS has no such symbol, which is how this
// first reached CI: the Linux and Windows builds were green and
// osx-universal-build failed to compile main.cpp outright. The portable
// spelling is `pipe` plus two `fcntl(F_SETFD)` calls; the only thing lost is
// atomicity against a concurrent `fork` in another thread, and this runs at
// the top of main() before any thread or child exists.
//
// Returns true and fills `fds` on success; on failure nothing is left open.
static bool createCloexecPipe(int fds[2]) {
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) == 0) {
        return true;
    }
    fds[0] = fds[1] = -1;
    return false;
#else
    if (::pipe(fds) != 0) {
        fds[0] = fds[1] = -1;
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(fds[i], F_GETFD, 0);
        if (flags < 0 || ::fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            fds[0] = fds[1] = -1;
            return false;
        }
    }
    return true;
#endif
}

static void shutdownSupervisor() {
    // Loops. It used to return after the FIRST signal, which left a window:
    // if that signal arrived before Crow had assigned its server (so
    // app.stop() was a no-op), the process carried on serving with no
    // supervisor left to read a second byte - terminable only by SIGKILL.
    // It now keeps handling signals until the write end is closed, which is
    // main's way of saying the process is leaving.
    for (;;) {
        char byte = 0;
        const ssize_t n = ::read(g_shutdown_pipe_read, &byte, 1);
        if (n == 1) {
            performShutdown(static_cast<int>(static_cast<unsigned char>(byte)));
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;   // interrupted, or a spurious wakeup; keep waiting
        }
        return;   // write end closed: clean exit, nothing more to do
    }
}
#endif

void signal_handler(int signal) {
    if (signal != SIGINT && signal != SIGTERM) {
        return;
    }
    should_exit.store(true, std::memory_order_relaxed);
#ifndef _WIN32
    // Read once into a local: the value must not be re-checked after the
    // test, or teardown could close it in between.
    const int fd = g_shutdown_pipe_write.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char byte = static_cast<char>(signal);
        // EAGAIN on a non-blocking pipe means the pipe is full, i.e. a
        // shutdown is already pending and the supervisor has not drained it
        // yet - nothing more to do. The write end is non-blocking precisely so
        // this cannot block inside a signal handler.
        const ssize_t written = ::write(fd, &byte, 1);
        (void)written;
    }
#else
    // Windows runs console handlers on a dedicated thread, so there is no
    // self-join hazard and no async-signal-safety constraint to respect.
    performShutdown(signal);
#endif
}

// Identity for the feedback banner and the issue link on error payloads.
static constexpr datazoo::BannerInfo kBanner {"flapi", FLAPI_VERSION,
                                              "https://github.com/DataZooDE/flapi"};

int main(int argc, char* argv[]) 
{
    std::set_terminate(terminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(windowsExceptionHandler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    // O_CLOEXEC on both ends: the fds must not leak into a subprocess, or a
    // child holding the write end open defeats the close-the-write-end wakeup
    // on a clean exit.
    //
    // O_NONBLOCK on the WRITE end only. write(2) in a signal handler must
    // never block, and a full pipe simply means a shutdown is already pending.
    // The READ end stays blocking: that is how the supervisor waits. Setting
    // it non-blocking made read() return EAGAIN immediately, the supervisor
    // treated that as "pipe closed" and exited during startup, and SIGTERM
    // then did nothing at all - caught by the offload suite's termination
    // tests going red.
    int shutdown_pipe[2] = {-1, -1};
    bool shutdown_pipe_ready = createCloexecPipe(shutdown_pipe);
    if (shutdown_pipe_ready) {
        const int flags = ::fcntl(shutdown_pipe[1], F_GETFL, 0);
        if (flags < 0 || ::fcntl(shutdown_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(shutdown_pipe[0]);
            ::close(shutdown_pipe[1]);
            shutdown_pipe_ready = false;
        } else {
            g_shutdown_pipe_read = shutdown_pipe[0];
            g_shutdown_pipe_write.store(shutdown_pipe[1], std::memory_order_release);
        }
    }
    if (!shutdown_pipe_ready) {
        shutdown_pipe[0] = shutdown_pipe[1] = -1;
        CROW_LOG_ERROR << "could not create the shutdown pipe; SIGINT/SIGTERM keep "
                          "their default disposition and will terminate the process "
                          "immediately, without draining in-flight requests";
    }
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART: without it the signal makes every blocking syscall in every
    // thread return EINTR at once, and code that does not check for it -
    // inside DuckDB, asio and the C++ runtime - misreads a partial or failed
    // operation as a real one. Measured: SIGTERM delivered to all threads
    // aborted the process with "corrupted double-linked list" rather than
    // shutting it down. The handler only sets a flag and writes a byte, so
    // there is nothing here that needs an interrupted syscall to observe.
    sa.sa_flags = SA_RESTART;
    // Only install the handler if there is something for it to signal.
    // Installing an inert handler would make the process IGNORE SIGTERM
    // entirely - strictly worse than the in-handler shutdown this replaced,
    // and worse than the default disposition.
    // RAII, because main() returns from a dozen places - every CLI subcommand
    // (`pack`, `unpack`, `info`), every argument-parsing error, and the server
    // path itself. A joinable std::thread whose destructor runs calls
    // std::terminate, so starting the supervisor early (which is what closes
    // the SIGTERM-during-startup window) turned `flapi pack` into an abort.
    // Caught by test_self_packaging.py and test_security_warnings.py.
    struct SupervisorGuard {
        std::thread thread;
        ~SupervisorGuard() {
            if (!thread.joinable()) {
                return;
            }
            // Closing the write end wakes a supervisor still blocked in
            // read(); one already running performShutdown is simply waited
            // for, which is the point - the process must not exit out from
            // under a drain in progress.
            const int fd = g_shutdown_pipe_write.exchange(-1, std::memory_order_acq_rel);
            if (fd >= 0) {
                ::close(fd);
            }
            thread.join();
        }
    } supervisor_guard;
    std::thread& shutdown_supervisor = supervisor_guard.thread;

    if (shutdown_pipe_ready) {
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        // Started HERE, not after the server is up. Between installing the
        // handler and starting the supervisor, a signal writes a byte nobody
        // reads - and the default disposition is already gone, so the process
        // ignores SIGTERM until SIGKILL. Config load and DB init sit in that
        // window and can take seconds.
        shutdown_supervisor = std::thread(shutdownSupervisor);
    }
#endif

    static argparse::ArgumentParser program("flapi");

    program.add_argument("-c", "--config")
        .help("Path to the flapi.yaml configuration file")
        .default_value(std::string("flapi.yaml"));

    program.add_argument("-p", "--port")
        .help("Port number for the web server")
        .default_value(-1)
        .scan<'i', int>();

    program.add_argument("--host")
        .help("Bind address for the web server (e.g. 0.0.0.0, 127.0.0.1)")
        .default_value(std::string(""));

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

    program.add_argument("--no-telemetry")
        .help("Disable telemetry (startup/shutdown events)")
        .default_value(false)
        .implicit_value(true);

    // ------------------------------------------------------------------
    // Self-packaging subcommands (#45). Each subcommand is optional; if
    // none is given the binary runs as the API server, preserving the
    // pre-existing `./flapi -c flapi.yaml` invocation.
    // ------------------------------------------------------------------

    argparse::ArgumentParser pack_cmd("pack");
    pack_cmd.add_description(
        "Package a flapi config tree into a self-contained executable.");
    pack_cmd.add_argument("--in")
        .help("Directory containing flapi.yaml + sqls/ + data/")
        .required();
    pack_cmd.add_argument("--out")
        .help("Output path for the new bundled executable")
        .required();
    pack_cmd.add_argument("--allow-secrets")
        .help("Bundle files matching the default secret exclude list "
              "(*.env, secrets/*, *.pem, *.key). Testing only.")
        .default_value(false)
        .implicit_value(true);
    pack_cmd.add_argument("--macos-append")
        .help("macOS only: append the archive after __LINKEDIT instead of "
              "overwriting the reserved __FLAPI/__bundle segment. "
              "Result is ad-hoc signed and NOT notarisable.")
        .default_value(false)
        .implicit_value(true);
    program.add_subparser(pack_cmd);

    argparse::ArgumentParser info_cmd("info");
    info_cmd.add_description(
        "Print bundle info (offset, size, entries) for this binary.");
    program.add_subparser(info_cmd);

    argparse::ArgumentParser unpack_cmd("unpack");
    unpack_cmd.add_description(
        "Dump the bundle of this binary into a directory.");
    unpack_cmd.add_argument("--to")
        .help("Destination directory (will be created if missing)")
        .required();
    program.add_subparser(unpack_cmd);

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    // ------------------------------------------------------------------
    // Subcommand dispatch. Returns early; never reaches server startup.
    // ------------------------------------------------------------------
    if (program.is_subcommand_used("pack")) {
        try {
            PackOptions opts;
            opts.allow_secrets = pack_cmd.get<bool>("--allow-secrets");
            opts.macos_mode = pack_cmd.get<bool>("--macos-append")
                ? MacOSPackMode::kAppend
                : MacOSPackMode::kReservedSegment;
            const auto in_dir = pack_cmd.get<std::string>("--in");
            const auto out_path = pack_cmd.get<std::string>("--out");
            const auto result = Pack(in_dir, out_path, opts);
            std::cout << "Packed " << result.entry_count
                      << " entries (" << result.archive_size
                      << " bytes) into " << result.output.string() << "\n";
            return 0;
        } catch (const PackError& e) {
            std::cerr << "flapi pack: " << e.what() << "\n";
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "flapi pack: unexpected error: " << e.what() << "\n";
            return 1;
        }
    }

    if (program.is_subcommand_used("info")) {
        return PrintBundleInfo(std::cout);
    }

    if (program.is_subcommand_used("unpack")) {
        try {
            const auto dst = unpack_cmd.get<std::string>("--to");
            const auto result = UnpackBundle(dst);
            std::cout << "Unpacked " << result.entries_written
                      << " entries to " << dst << "\n";
            return 0;
        } catch (const PackError& e) {
            std::cerr << "flapi unpack: " << e.what() << "\n";
            return 1;
        }
    }

    std::string config_file = program.get<std::string>("--config");
    int cmd_port = program.get<int>("--port");
    std::string cmd_host = program.get<std::string>("--host");
    std::string log_level = program.get<std::string>("--log-level");
    bool validate_config = program.get<bool>("--validate-config");

    // 12-factor env-var fallback (#47, #63). Precedence:
    //   CLI flag > env var > config file > built-in default.
    // CLI wins because we only consult the env when the user didn't
    // pass the flag; config-file values are applied later in
    // initializeConfig() and only kick in when neither CLI nor env
    // provided a value.
    if (!program.is_used("--config")) {
        if (const char* env = std::getenv("FLAPI_CONFIG"); env != nullptr && *env != '\0') {
            config_file = env;
        }
    }
    if (!program.is_used("--log-level")) {
        if (const char* env = std::getenv("FLAPI_LOG_LEVEL"); env != nullptr && *env != '\0') {
            log_level = env;
        }
    }
    if (!program.is_used("--port")) {
        if (const char* env = std::getenv("FLAPI_PORT"); env != nullptr && *env != '\0') {
            // Reject non-int / out-of-range early so a typo doesn't
            // silently fall through to the config-file value.
            try {
                size_t consumed = 0;
                const int parsed = std::stoi(env, &consumed);
                if (consumed != std::strlen(env) || parsed < 1 || parsed > 65535) {
                    throw std::invalid_argument("out of range");
                }
                cmd_port = parsed;
            } catch (const std::exception&) {
                std::cerr << "flapi: invalid FLAPI_PORT '" << env
                          << "'; must be an integer in 1..65535\n";
                return 1;
            }
        }
    }
    if (!program.is_used("--host")) {
        if (const char* env = std::getenv("FLAPI_HOST"); env != nullptr && *env != '\0') {
            cmd_host = env;
        }
    }
    // Validate log_level. Invalid values are an error, not a silent
    // fallback -- typos like FLAPI_LOG_LEVEL=DEBUG should surface
    // immediately, not run the server at the wrong verbosity.
    if (log_level != "debug" && log_level != "info" &&
        log_level != "warning" && log_level != "error") {
        std::cerr << "flapi: invalid log level '" << log_level
                  << "'; must be one of: debug, info, warning, error\n";
        return 1;
    }
    bool config_service_enabled = program.get<bool>("--config-service");
    std::string config_service_token = program.get<std::string>("--config-service-token");
    bool no_telemetry = program.get<bool>("--no-telemetry");

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

    // Apply the CLI/env level now so configuration loading itself is logged at
    // the requested verbosity; the config file can lower it further below.
    set_log_level(log_level);

    detectAndRegisterEmbeddedBundle();

    // A configuration error is a user error, not a crash. This used to be an
    // uncaught throw: terminateHandler logged it and called std::abort(), so a
    // typo in flapi.yaml raised SIGABRT, dumped core, and exited 134. In a
    // container that is reported as a crash - indistinguishable from a real
    // fault - and on a host with core dumps enabled every restart attempt
    // wrote one, of a ~77 MB binary.
    //
    // terminateHandler stays for genuinely unexpected exceptions; that is what
    // it is for. Startup config loading is not one of them (#126).
    std::shared_ptr<ConfigManager> config_manager;
    try {
        config_manager = initializeConfig(config_file);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Configuration error: " << e.what();
        CROW_LOG_ERROR << "flAPI cannot start until the configuration is valid.";
        return 1;
    }

    // Precedence: CLI > environment > config file > default. An operator who
    // passed --log-level meant it, but absent that the config file must be
    // honoured - three example files shipped a log level that did nothing.
    const bool log_level_from_cli_or_env =
        program.is_used("--log-level") ||
        (std::getenv("FLAPI_LOG_LEVEL") != nullptr && *std::getenv("FLAPI_LOG_LEVEL") != '\0');
    if (!log_level_from_cli_or_env && config_manager) {
        const std::string& configured = config_manager->getLogLevel();
        if (!configured.empty() && configured != log_level) {
            log_level = configured;
            set_log_level(log_level);
        }
    }

    // Tracing lifecycle. configure() is a no-op unless an operator explicitly
    // enabled it: an injected OTEL_EXPORTER_OTLP_ENDPOINT is a platform default,
    // not consent to ship data off the machine (BR-6).
    //
    // The guard shuts the provider down deterministically at scope exit. A
    // BatchSpanProcessor owns an export thread, and letting static destruction
    // race it is an intermittent crash at exit - which is why Tracing(), unlike
    // GlobalTelemetry(), is not a leaked singleton.
    flapi::TracingGuard tracing_guard;
    if (config_manager) {
        const auto& tracing_config = config_manager->getTracingConfig();
#if !FLAPI_WITH_TRACING
        if (tracing_config.enabled) {
            CROW_LOG_WARNING << "tracing.enabled is set, but this binary was built "
                                "with FLAPI_WITH_TRACING=OFF - no spans will be produced";
        }
#endif
        flapi::Tracing().configure(tracing_config);
        if (flapi::Tracing().isEnabled()) {
            CROW_LOG_INFO << "Tracing enabled: exporter=" << tracing_config.exporter
                          << " capture=" << flapi::captureTierName(tracing_config.capture);
            if (tracing_config.capture == flapi::CaptureTier::Payload) {
                // Payload capture exports customer data. An operator must not be
                // able to reach that without seeing it said out loud.
                CROW_LOG_WARNING << "Tracing capture tier is PAYLOAD: argument values and "
                                    "result rows will be exported. Ensure this is intended.";
            }
        }
    }

    // Install the correlating log handler. Every existing CROW_LOG_* call site -
    // and every future one - gains the request id without being touched.
    static flapi::FlapiLogHandler log_handler(
        config_manager && config_manager->getLogFormat() == "json"
            ? flapi::FlapiLogHandler::Format::Json
            : flapi::FlapiLogHandler::Format::Text);
    crow::logger::setHandler(&log_handler);

    // Surface configuration-level security warnings (plaintext passwords, MCP without auth, etc.)
    // Runs in both --validate-config mode and normal server start; never aborts startup.
    {
        SecurityAuditor auditor;
        printSecurityWarnings(auditor.audit(*config_manager));
    }

    // If validate-config flag is set, validate and exit
    if (validate_config) {
        return validateConfiguration(config_manager, config_file);
    }

    // Resolve no_telemetry: CLI flag > FLAPI_NO_TELEMETRY env > config file
    if (!no_telemetry) {
        const char* env_val = std::getenv("FLAPI_NO_TELEMETRY");
        if (env_val) {
            std::string s(env_val);
            no_telemetry = (s == "1" || s == "true" || s == "yes");
        }
    }
    if (!no_telemetry && !config_manager->isTelemetryEnabled()) {
        no_telemetry = true;
    }

    // Initialize cloud storage credentials (reads environment variables)
    initializeCloudCredentials();

    if (cmd_port != -1) {
        config_manager->setHttpPort(cmd_port);
    }
    if (!cmd_host.empty()) {
        config_manager->setHttpHost(cmd_host);
    }

    initializeDatabase(config_manager);

    // If a bundle was detected at startup, register the embed:// FS
    // on the DuckDB instance so `read_csv('embed://...')` and similar
    // calls inside SQL templates can resolve to the in-memory archive.
    if (RegisterEmbeddedFileSystem()) {
        CROW_LOG_INFO << "Registered embed:// DuckDB filesystem";
    }

    // Configure cloud credentials in DuckDB after database is initialized
    configureCloudCredentialsInDuckDB();

    // Verify storage health at startup
    verifyStorageHealth(config_manager);

    // Create unified API server with MCP support (always enabled in unified configuration)
    setApiServer(std::make_shared<APIServer>(
        config_manager,
        DatabaseManager::getInstance(),
        config_service_enabled,
        config_service_token
    ));

    // A SIGTERM during config load or DB init - which can take seconds - has
    // already been acted on by the supervisor, against a server that did not
    // exist yet. Do not then bring one up and start serving.
    if (should_exit.load(std::memory_order_relaxed)) {
        CROW_LOG_INFO << "shutdown requested during startup; not starting the server";
        if (auto server = getApiServer()) {
            server->stop();
        }
        return 0;   // supervisor_guard winds the supervisor down
    }

    // Initialize telemetry (this is a long-running server: install_kind="server",
    // one $session_id per uptime) and emit server_started. A single opt-out —
    // CLI flag, env, or YAML, already resolved into no_telemetry — disables
    // everything via setEnabled(false).
    {
        auto& telemetry = flapi::GlobalTelemetry();
        telemetry.setEnabled(!no_telemetry);
        telemetry.setSampling(config_manager->getTelemetrySampleRate());

        const char* edition_env = std::getenv("FLAPI_EDITION");
        const std::string edition =
            (edition_env != nullptr && *edition_env != '\0') ? edition_env : "oss";
        telemetry.configureProduct(FLAPI_VERSION, edition);
        telemetry.associateDeployment();
        if (const char* lic = std::getenv("FLAPI_LICENSE_ID");
            lic != nullptr && *lic != '\0') {
            telemetry.associateAccount(lic);
        }
        telemetry.serverStarted(
            static_cast<int>(config_manager->getEndpoints()->size()),
            deriveAuthKind(*config_manager));
    }

    // Start unified server
    std::thread unified_server_thread([config_manager, server = api_server]() {
        // Caught here. run() surfaces a startup failure as an exception -
        // EADDRINUSE, an unresolvable bind address, a failed validate() - and
        // an exception escaping a std::thread's function calls
        // std::terminate, so the process died with SIGABRT without draining
        // the handler pool or running stop() for ANY of them.
        try {
            server->run(config_manager->getHttpPort());
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "the server could not start: " << e.what();
            should_exit.store(true, std::memory_order_relaxed);
            server->stop();
        }
    });

    CROW_LOG_INFO << "flAPI unified server started - REST API and MCP on port " << config_manager->getHttpPort();

    std::thread warmup_thread;
    if (auto cache_manager = DatabaseManager::getInstance()->getCacheManager()) {
        warmup_thread = cache_manager->warmUpCachesAsync(config_manager);
    }

    // Once-a-day feedback nudge, at the point the server is actually up. Needs
    // both streams to be terminals, so a container or systemd start -- how this
    // runs in production -- prints nothing. The log line above remains the
    // operator-facing signal.
    datazoo::ShowBannerStandalone(kBanner);
    CROW_LOG_INFO << datazoo::FeedbackLine(kBanner);
    
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

    // Drain and join the handler pool HERE, not in ~APIServer. The destructor
    // runs during static destruction, by which point QueryExecutor's
    // function-local statics are gone - and a worker still inside a query
    // reaches them and segfaults. stop() is idempotent, so this costs nothing
    // on the signalled path where the supervisor already ran it.
    if (auto server = getApiServer()) {
        server->stop();
    }


    if (warmup_thread.joinable()) {
        warmup_thread.join();
    }

    // Drain buffered telemetry on clean exit; the signal path already flushed.
    if (!should_exit) {
        flapi::GlobalTelemetry().flush();
    }

    return 0;
}

#pragma once

#include <crow/logging.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace flapi {

struct RequestContext;

// Correlates every CROW_LOG_* line with the request that produced it.
//
// flAPI has ~658 logging call sites. Rather than edit them - a huge diff that new
// code would immediately diverge from - this installs one crow::ILogHandler that
// reads the ambient RequestContext and stamps the identity on its way out. Every
// existing line and every future line is correlated, with no call-site churn.
//
// Two output shapes:
//   Text - Crow's usual human-readable line with `request_id=...` appended
//   Json - one JSON object per line, for a log shipper
//
// The handler is installed once and called concurrently from every pooled Crow
// worker, so it builds each record completely and writes it in a single
// operation under a mutex. Writing in pieces would let two threads interleave
// into unparseable output.
class FlapiLogHandler : public crow::ILogHandler {
public:
    enum class Format { Text, Json };

    explicit FlapiLogHandler(Format format);

    // `hold`: HOLD every line until setFormat() is called. Installed before
    // the configuration is read, so the lines logged while loading it come
    // out in the `log-format` the configuration asks for: with json
    // configured, the first startup lines used to be plain text, which a log
    // shipper cannot parse. Each held line keeps the time it was logged at.
    FlapiLogHandler(Format format, bool hold);
    // Lines still held at exit (std::exit before the format was settled) are
    // written in the current format rather than lost.
    ~FlapiLogHandler() override;

    // Fix the format and write out every held line. Idempotent; later calls
    // only change the format.
    void setFormat(Format format);

    void log(const std::string& message, crow::LogLevel level) override;

    // Exposed for tests: asserting on the exact rendered line is the only way to
    // prove a key is absent rather than merely empty.
    std::string formatForTest(const std::string& message, crow::LogLevel level) const {
        return format(message, level, currentTimestamp(), currentRequestIdOverride());
    }

    // The override in force on this thread; empty when none.
    static std::string_view currentRequestIdOverride() noexcept;

    static const char* levelName(crow::LogLevel level);

    // Crow logs "Response: <conn> <url> <status>" itself, at a point where the
    // request's context may no longer be ambient: a middleware that refuses a
    // request (401/403/429) unwinds through RequestContextMiddleware's
    // after_handle - which clears the context - BEFORE Crow logs the response.
    // RequestContextMiddleware::finish() calls this; when the Response line of
    // `rc` has not been written yet, the id is kept for the next Response line
    // on this thread (which is that request's, written immediately after).
    static void noteRequestFinished(const RequestContext* rc, std::string_view request_id);
    // Before a new request starts on this thread: nothing pending is its.
    static void resetPendingResponse();

    // Crow also logs "Request: <conn> <url> HTTP/1.1 GET /path" before any
    // middleware runs, so it has no request context to take an id from. log()
    // holds that one line (per thread) instead of writing it;
    // RequestContextMiddleware::before_handle calls this once the new request is
    // ambient, and the line is written then, with its request_id. A held line
    // that is never released (before_handle did not run) is written unchanged
    // by the next log call on the thread.
    static void releasePendingRequestLine();

    // Attributes the lines logged on this thread, while it lives, to `request_id`
    // instead of the ambient request (or to none). For a writer that knows which
    // request a line belongs to although that request is not ambient on its
    // thread: the DuckDB HTTP request log runs on DuckDB's worker threads, and
    // marks the lines no request caused with "-". The id is stamped by the
    // handler like any other, so it appears once, as `request_id=` in text and
    // as the `request_id` field in JSON. When it is the ambient request's own
    // id, that request's trace ids are kept.
    class RequestIdOverride {
    public:
        explicit RequestIdOverride(std::string_view request_id) noexcept;
        ~RequestIdOverride() noexcept;
        RequestIdOverride(const RequestIdOverride&) = delete;
        RequestIdOverride& operator=(const RequestIdOverride&) = delete;

    private:
        std::string_view previous_;
    };

private:
    static std::string currentTimestamp();
    std::string format(const std::string& message, crow::LogLevel level, const std::string& timestamp,
                       std::string_view request_id_override) const;

    struct HeldLine {
        std::string message;
        crow::LogLevel level;
        std::string timestamp;
        std::string request_id_override;   // a held line keeps the override it was logged under
    };

    std::atomic<Format> format_;
    std::atomic<bool> holding_{false};
    std::vector<HeldLine> held_;   // guarded by write_mutex_
    mutable std::mutex write_mutex_;
};

}  // namespace flapi

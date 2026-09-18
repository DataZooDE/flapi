#pragma once

#include <crow/logging.h>

#include <mutex>
#include <string>

namespace flapi {

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

    void log(std::string message, crow::LogLevel level) override;

    // Exposed for tests: asserting on the exact rendered line is the only way to
    // prove a key is absent rather than merely empty.
    std::string formatForTest(const std::string& message, crow::LogLevel level) const {
        return format(message, level);
    }

    static const char* levelName(crow::LogLevel level);

private:
    std::string format(const std::string& message, crow::LogLevel level) const;

    Format format_;
    mutable std::mutex write_mutex_;
};

}  // namespace flapi

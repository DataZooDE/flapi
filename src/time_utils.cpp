#include "time_utils.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace flapi {

std::string nowIso8601Utc() {
    const auto now = std::chrono::system_clock::now();
    const auto now_t = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count() % 1'000'000;
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &now_t);
#else
    gmtime_r(&now_t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(6) << micros << 'Z';
    return oss.str();
}

}  // namespace flapi

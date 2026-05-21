#include "selfpath.hpp"

#include <cerrno>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <cstdint>
#else
    #include <unistd.h>
#endif

namespace flapi {

std::filesystem::path GetSelfPath() {
#ifdef _WIN32
    std::vector<wchar_t> buf(MAX_PATH);
    while (true) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "GetModuleFileNameW failed");
        }
        if (n < buf.size()) {
            return std::filesystem::path(std::wstring(buf.data(), n));
        }
        // Buffer too small (MS docs say n == buf.size() means truncated on
        // older Windows; on newer it sets ERROR_INSUFFICIENT_BUFFER).
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    // First call sizes the buffer.
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "_NSGetExecutablePath failed");
    }
    std::error_code ec;
    auto canonical = std::filesystem::canonical(std::filesystem::path(buf.data()), ec);
    if (ec) {
        return std::filesystem::path(buf.data());
    }
    return canonical;
#else
    std::error_code ec;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        throw std::system_error(ec, "read_symlink(/proc/self/exe) failed");
    }
    return resolved;
#endif
}

}  // namespace flapi

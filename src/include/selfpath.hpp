#pragma once

#include <filesystem>

namespace flapi {

// Returns the absolute path of the currently running executable.
// Implemented per-OS:
//   - Linux:   readlink("/proc/self/exe")
//   - macOS:   _NSGetExecutablePath
//   - Windows: GetModuleFileNameW
// Throws std::system_error on resolution failure.
std::filesystem::path GetSelfPath();

}  // namespace flapi

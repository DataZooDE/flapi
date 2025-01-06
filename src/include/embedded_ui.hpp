#pragma once

#include <string>
#include <string_view>

namespace flapi {
namespace embedded_ui {

// Get the content of an embedded file
std::string_view get_file_content(const std::string& path);

} // namespace embedded_ui
} // namespace flapi 
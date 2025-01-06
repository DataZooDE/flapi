#include "embedded_ui.hpp"
#include "embedded/index_html.hpp"

namespace flapi {
namespace embedded_ui {

std::string_view get_file_content(const std::string& path) {
    // Since we're using a single file approach, all paths return the same content
    return std::string_view(reinterpret_cast<const char*>(index_html), index_html_len);
}

} // namespace embedded_ui
} // namespace flapi 
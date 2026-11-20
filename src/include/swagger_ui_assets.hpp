#pragma once

#include <cstddef>
#include <string_view>

namespace flapi {

// A file of the vendored swagger-ui-dist (third_party/swagger-ui-dist, pinned
// 5.18.2), compiled into the binary so `/doc` needs no network access.
struct SwaggerUiAsset {
    const char* name;           // as served: /doc/assets/<name>
    const char* content_type;
    const unsigned char* data;
    std::size_t size;
};

// The asset served as /doc/assets/<name>, or nullptr. An exact match on a fixed
// list: nothing derived from the request ever reaches the file system.
const SwaggerUiAsset* FindSwaggerUiAsset(std::string_view name);

}  // namespace flapi

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <yaml-cpp/yaml.h>

#include "yaml_text_patch.hpp"

using flapi::PatchYamlPreservingComments;

namespace {

YAML::Node DeepMerge(const YAML::Node& base, const YAML::Node& patch) {
    if (!base || !base.IsMap() || !patch.IsMap()) {
        return YAML::Clone(patch);
    }
    YAML::Node out = YAML::Clone(base);
    for (const auto& kv : patch) {
        const std::string key = kv.first.as<std::string>();
        if (out[key] && out[key].IsMap() && kv.second.IsMap()) {
            out[key] = DeepMerge(out[key], kv.second);
        } else {
            out[key] = YAML::Clone(kv.second);
        }
    }
    return out;
}

std::optional<std::string> Patch(const std::string& text, const std::string& patch_yaml) {
    const YAML::Node base = YAML::Load(text);
    return PatchYamlPreservingComments(text, base, DeepMerge(base, YAML::Load(patch_yaml)));
}

const char* kDoc =
    "# header comment\n"
    "url-path: /items\n"
    "method: GET   # read only\n"
    "\n"
    "# the connection\n"
    "connection: [inmem]\n"
    "request:\n"
    "  # paging\n"
    "  - field-name: limit   # the page size\n"
    "    field-in: query\n"
    "mcp-tool:\n"
    "  name: list_items\n"
    "  # what agents read\n"
    "  description: First   # old\n"
    "\n"
    "# trailing\n";

}  // namespace

TEST_CASE("YAML patch keeps comments around an unchanged document", "[yaml_text_patch]") {
    const auto out = Patch(kDoc, "url-path: /items");
    REQUIRE(out.has_value());
    CHECK(*out == kDoc);
}

TEST_CASE("YAML patch rewrites only the changed nested key", "[yaml_text_patch]") {
    const auto out = Patch(kDoc, "mcp-tool: {description: Second}");
    REQUIRE(out.has_value());
    CHECK(out->find("# header comment") != std::string::npos);
    CHECK(out->find("# read only") != std::string::npos);
    CHECK(out->find("# the connection") != std::string::npos);
    CHECK(out->find("# paging") != std::string::npos);
    CHECK(out->find("# the page size") != std::string::npos);
    CHECK(out->find("# what agents read") != std::string::npos);
    CHECK(out->find("# trailing") != std::string::npos);
    CHECK(out->find("# old") != std::string::npos);   // inline comment of the changed scalar
    CHECK(out->find("description: Second") != std::string::npos);
    CHECK(out->find("First") == std::string::npos);
    CHECK(YAML::Load(*out)["mcp-tool"]["name"].as<std::string>() == "list_items");
}

TEST_CASE("YAML patch appends a new key after the last entry", "[yaml_text_patch]") {
    const auto out = Patch(kDoc, "rate-limit: {max: 5}");
    REQUIRE(out.has_value());
    CHECK(out->find("# trailing") != std::string::npos);
    CHECK(out->find("rate-limit:") != std::string::npos);
    CHECK(YAML::Load(*out)["rate-limit"]["max"].as<int>() == 5);
    // the new key sits before the trailing comment, after the last entry
    CHECK(out->find("rate-limit:") < out->find("# trailing"));
}

TEST_CASE("YAML patch replaces a changed sequence as a whole", "[yaml_text_patch]") {
    const auto out = Patch(kDoc, "request: [{field-name: id, field-in: path}]");
    REQUIRE(out.has_value());
    CHECK(out->find("# header comment") != std::string::npos);
    CHECK(out->find("# read only") != std::string::npos);
    const auto doc = YAML::Load(*out);
    REQUIRE(doc["request"].size() == 1);
    CHECK(doc["request"][0]["field-name"].as<std::string>() == "id");
}

TEST_CASE("YAML patch falls back on layouts it does not handle", "[yaml_text_patch]") {
    // document marker
    CHECK_FALSE(Patch("---\na: 1\n", "a: 2").has_value());
    // quoted key
    CHECK_FALSE(Patch("\"a\": 1\nb: 2\n", "b: 3").has_value());
    // CRLF
    CHECK_FALSE(Patch("a: 1\r\nb: 2\r\n", "b: 3").has_value());
}

TEST_CASE("YAML patch result always parses to the requested document", "[yaml_text_patch]") {
    const std::string text = "a: 1\nb:\n  c: 2\n  d: [1, 2]\ne: |\n  literal\n  text\nf: 7\n";
    const YAML::Node base = YAML::Load(text);
    const YAML::Node merged = DeepMerge(base, YAML::Load("b: {c: 9}\nf: 8\ng: new"));
    const auto out = PatchYamlPreservingComments(text, base, merged);
    REQUIRE(out.has_value());
    const auto doc = YAML::Load(*out);
    CHECK(doc["b"]["c"].as<int>() == 9);
    CHECK(doc["b"]["d"].size() == 2);
    CHECK(doc["e"].as<std::string>() == "literal\ntext\n");
    CHECK(doc["f"].as<int>() == 8);
    CHECK(doc["g"].as<std::string>() == "new");
}

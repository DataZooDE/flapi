// #211 round 1, cluster F2: checking a rendered template without running it
// (template/expand?validate_only, with and without target=cache).
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "mustache_syntax_check.hpp"
#include <crow/mustache.h>
#include <set>

using namespace flapi;
namespace fs = std::filesystem;

namespace {

bool HasIssue(const std::vector<DatabaseManager::ScriptIssue>& issues, std::size_t statement, const std::string& type) {
    for (const auto& issue : issues) {
        if (issue.statement == statement && issue.type == type) {
            return true;
        }
    }
    return false;
}

std::string Dump(const DatabaseManager::ScriptValidation& v) {
    std::string out;
    for (const auto& e : v.errors) {
        out += "E" + std::to_string(e.statement) + " " + e.type + ": " + e.message + "\n";
    }
    for (const auto& w : v.warnings) {
        out += "W" + std::to_string(w.statement) + " " + w.type + ": " + w.message + "\n";
    }
    return out;
}

int64_t Count(DatabaseManager& db, const std::string& sql) {
    std::map<std::string, std::string> params;
    auto result = db.executeQuery(sql, params, false);
    const std::string dump = result.data.dump();
    // [{"n":<value>}]
    const auto colon = dump.find(':');
    return std::stoll(dump.substr(colon + 1));
}

}  // namespace

TEST_CASE("SplitStatements splits on top-level semicolons only", "[f2][validate]") {
    using V = std::vector<std::string>;
    REQUIRE(DatabaseManager::SplitStatements("SELECT 1; SELECT 2;") == V{"SELECT 1", " SELECT 2"});
    REQUIRE(DatabaseManager::SplitStatements("SELECT ';' AS a; SELECT \"x;y\" FROM t").size() == 2);
    REQUIRE(DatabaseManager::SplitStatements("SELECT 'it''s; here'").size() == 1);
    REQUIRE(DatabaseManager::SplitStatements("-- a; comment\nSELECT 1 /* ; */").size() == 1);
    REQUIRE(DatabaseManager::SplitStatements("SELECT $$a;b$$; SELECT $t$;$t$").size() == 2);
    // Comment-only and empty pieces are no statement.
    REQUIRE(DatabaseManager::SplitStatements("SELECT 1;\n-- trailing note\n;  ;").size() == 1);
    REQUIRE(DatabaseManager::SplitStatements("  \n").empty());
    // A statement that does not parse still gets its own number.
    REQUIRE(DatabaseManager::SplitStatements("CREATE TABLE t (a INT); MERGE INTOO t; SELEC oops;").size() == 3);
}

TEST_CASE("A bare SELECT before the last statement is called a discarded guard", "[f2][validate]") {
    const PopulateTarget target{"cache", "main", "orders"};
    const auto guard = DatabaseManager::PopulateSelectError(target, 2, 3);
    REQUIRE(guard.find("statement 2 of 3 is a bare SELECT; its result is discarded") != std::string::npos);
    REQUIRE(guard.find("CREATE OR REPLACE TEMP TABLE _chk AS SELECT") != std::string::npos);
    REQUIRE(guard.find("cache.main.orders") != std::string::npos);
    REQUIRE(guard.find("builds nothing") == std::string::npos);

    // The last (or only) statement: the old message, which is right there.
    const auto last = DatabaseManager::PopulateSelectError(target, 3, 3);
    REQUIRE(last.find("builds nothing") != std::string::npos);
    REQUIRE(DatabaseManager::PopulateSelectError(target, 1, 1).find("builds nothing") != std::string::npos);

    std::size_t count = 0;
    REQUIRE(DatabaseManager::FindSelectStatement("CREATE TABLE c.main.t (a INT); SELECT 1; INSERT INTO c.main.t VALUES (1)",
                                                 &count) == std::optional<std::size_t>(2));
    REQUIRE(count == 3);
}

TEST_CASE("validateScript parses, binds and never runs a populate script", "[f2][validate][ducklake]") {
    const fs::path temp_dir = fs::temp_directory_path() / "flapi_f2_validate_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "templates");
    fs::create_directories(temp_dir / "cache_data");
    {
        std::ofstream(temp_dir / "templates" / "dummy.sql") << "SELECT 1";
        std::ofstream config(temp_dir / "flapi.yaml");
        config << "project-name: f2\nproject-description: f2\ntemplate:\n  path: " << (temp_dir / "templates").string()
               << "\nducklake:\n  enabled: true\n  alias: cache\n  metadata-path: "
               << (temp_dir / "metadata.ducklake").string() << "\n  data-path: " << (temp_dir / "cache_data").string()
               << "\nconnections:\n  default:\n    init: \"SELECT 1;\"\n";
    }
    auto config_manager = std::make_shared<ConfigManager>(temp_dir / "flapi.yaml");
    config_manager->loadConfig();
    auto db = DatabaseManager::getInstance();
    REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(config_manager));

    const PopulateTarget target{"cache", "main", "orders_cache"};

    SECTION("a syntax error is reported against its own statement") {
        const auto v = db->validateScript(
            "CREATE TABLE IF NOT EXISTS cache.main.orders_cache (id INTEGER);\nMERGE INTOO cache.main.orders_cache;",
            &target, &target);
        REQUIRE(HasIssue(v.errors, 2, "parse"));
        REQUIRE_FALSE(HasIssue(v.errors, 1, "parse"));
        REQUIRE(v.statements == 2);
    }

    SECTION("SELEC is a parse error, a bare SELECT a bare_select error") {
        REQUIRE(HasIssue(db->validateScript("SELEC oops;", &target, &target).errors, 1, "parse"));
        const auto v = db->validateScript(
            "CREATE OR REPLACE TABLE cache.main.orders_cache AS SELECT 1 AS id;\nSELECT count(*) FROM "
            "cache.main.orders_cache;\nINSERT INTO cache.main.orders_cache VALUES (2);",
            &target, &target);
        REQUIRE(HasIssue(v.errors, 2, "bare_select"));
        REQUIRE(v.errors.size() == 1);
        REQUIRE(v.errors[0].message.find("its result is discarded") != std::string::npos);
    }

    SECTION("an unknown column is a bind error even before the table exists") {
        // The first build: CREATE ... IF NOT EXISTS then the write. The CREATE
        // runs inside the rolled-back check, so the write is bound for real.
        const auto plain = db->validateScript(
            "CREATE TABLE IF NOT EXISTS cache.main.orders_cache (id INTEGER, v VARCHAR);\n"
            "INSERT INTO cache.main.orders_cache SELECT c_bogus FROM (SELECT 1 AS id) s;",
            &target, &target);
        INFO(Dump(plain));
        REQUIRE(HasIssue(plain.errors, 2, "bind"));
        REQUIRE(plain.errors[0].message.find("c_bogus") != std::string::npos);

        const auto ctas = db->validateScript(
            "CREATE TABLE IF NOT EXISTS cache.main.orders_cache AS SELECT 1 AS id WHERE false;\n"
            "MERGE INTO cache.main.orders_cache AS t USING (SELECT 1 AS id) AS s ON t.c_bogus = s.id "
            "WHEN NOT MATCHED THEN INSERT *;",
            &target, &target);
        INFO(Dump(ctas));
        REQUIRE(HasIssue(ctas.errors, 2, "bind"));

        // Nothing was committed: the table still does not exist.
        REQUIRE(Count(*db, "SELECT count(*) AS n FROM duckdb_tables() WHERE database_name = 'cache' AND table_name = "
                           "'orders_cache'") == 0);
    }

    SECTION("a correct script is valid and its DML is not executed") {
        std::map<std::string, std::string> params;
        db->executeQuery("CREATE OR REPLACE TABLE cache.main.keep (id INTEGER)", params, false);
        const PopulateTarget keep{"cache", "main", "keep"};
        const auto v = db->validateScript(
            "BEGIN TRANSACTION;\nDELETE FROM cache.main.keep WHERE id = 1;\nINSERT INTO cache.main.keep VALUES (1), (2);\n"
            "COMMIT;",
            &keep, &keep);
        INFO(Dump(v));
        REQUIRE(v.errors.empty());
        REQUIRE(Count(*db, "SELECT count(*) AS n FROM cache.main.keep") == 0);
        db->executeQuery("DROP TABLE cache.main.keep", params, false);
    }

    SECTION("a serving template over a cache not built yet is a warning") {
        const auto v = db->validateScript("SELECT * FROM cache.main.orders_cache ORDER BY id", nullptr, &target);
        INFO(Dump(v));
        REQUIRE(v.errors.empty());
        REQUIRE(HasIssue(v.warnings, 1, "cache_not_built"));

        const auto other = db->validateScript("SELECT nope FROM (SELECT 1 AS id)", nullptr, &target);
        REQUIRE(HasIssue(other.errors, 1, "bind"));
    }

    // Leave the singleton as later test files expect it: not initialised
    // (test_vfs_health.cpp relies on that; a live DuckDB there crashes it).
    db->reset();
    db.reset();
    config_manager.reset();
}

namespace {

std::set<std::string> Names(const std::vector<flapi::MustacheVariable>& variables) {
    std::set<std::string> out;
    for (const auto& v : variables) {
        out.insert(v.name);
    }
    return out;
}

crow::mustache::context SampleContext() {
    crow::mustache::context ctx;
    ctx["conn"]["path"] = "/data/x.parquet";
    ctx["params"]["name"] = "O'Brien";
    ctx["auth"]["authenticated"] = "";
    return ctx;
}

}  // namespace

TEST_CASE("validate_only: variables are found the way Mustache resolves them (#277)", "[f2][validate][variables]") {
    const auto ctx = SampleContext();
    const std::set<std::string> declared = {"name", "region", "limit", "offset"};

    SECTION("triple braces, sections, inverted sections, closes and comments name nothing undefined") {
        const std::string body =
            "SELECT * FROM read_parquet('{{{ conn.path }}}') WHERE 1=1 {{! a comment }}\n"
            "{{#params.name}} AND n = '{{ params.name }}' {{/params.name}}\n"
            "{{^params.region}} AND r = 'eu' {{/params.region}}";
        REQUIRE(flapi::FindUndefinedTemplateVariables(body, ctx, declared).empty());
    }
    SECTION("a declared parameter that was not passed is fine") {
        REQUIRE(flapi::FindUndefinedTemplateVariables("WHERE r = '{{ params.region }}'", ctx, declared).empty());
    }
    SECTION("a name relative to a section is not looked up at the top") {
        REQUIRE(flapi::FindUndefinedTemplateVariables("{{#params.name}}{{.}} {{ length }}{{/params.name}}", ctx,
                                                      declared)
                    .empty());
    }
    SECTION("auth.* is always defined, empty in the config service") {
        REQUIRE(flapi::FindUndefinedTemplateVariables("'{{{ auth.username }}}' '{{ auth.roles }}'", ctx, declared)
                    .empty());
    }
    SECTION("what is really missing is reported once, by its own name") {
        const auto found = flapi::FindUndefinedTemplateVariables(
            "{{ params.nope }} {{{ conn.nothere }}} {{#params.nope}}x{{/params.nope}} {{ env.UNSET }} {{ top }}", ctx,
            declared);
        REQUIRE(Names(found) == std::set<std::string>{"params.nope", "conn.nothere", "env.UNSET", "top"});
        REQUIRE(found.size() == 4);
        REQUIRE(found[0].line == 1);
        REQUIRE(found[0].column == 1);
    }
}

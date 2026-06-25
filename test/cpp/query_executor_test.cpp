#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "query_executor.hpp"
#include <cstdlib>

using namespace std;

namespace flapi {
namespace test {

TEST_CASE("QueryExecutor basic functionality", "[query_executor]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    
    SECTION("Simple integer query") {
        QueryExecutor executor(database);
        executor.execute("SELECT 42 as answer");
        auto doc = crow::json::load(executor.toJson().dump());

        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["answer"].i() == 42);
    }

    SECTION("NULL handling") {
        QueryExecutor executor(database);
        executor.execute("SELECT NULL as null_value");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);

        std::cout << "Null value: " << doc[0]["null_value"] << std::endl;
        REQUIRE(doc[0]["null_value"].t() == crow::json::type::Null);
    }

    SECTION("String handling") {
        QueryExecutor executor(database);
        executor.execute("SELECT 'hello world' as greeting");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["greeting"].s() == "hello world");
    }

    duckdb_close(&database);
}

TEST_CASE("QueryExecutor memory stress test", "[query_executor][memory]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    
    BENCHMARK("Memory usage under 10 iterations") {
        for (int i = 0; i < 10; i++) {
            QueryExecutor executor(database);
            executor.execute("SELECT {'nested': { 'array': [1,2,3], 'text': 'test' }} as complex");
            auto doc = crow::json::load(executor.toJson().dump());
            REQUIRE(doc.size() == 1);
        }
    };
    
    duckdb_close(&database);
}

TEST_CASE("QueryExecutor error handling", "[query_executor]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    
    SECTION("Invalid query throws") {
        QueryExecutor executor(database);
        REQUIRE_THROWS_AS(executor.execute("INVALID SQL"), runtime_error);
    }

    SECTION("Valid after invalid query") {
        QueryExecutor executor(database);
        REQUIRE_THROWS(executor.execute("INVALID SQL"));
        REQUIRE_NOTHROW(executor.execute("SELECT 1"));
    }

    duckdb_close(&database);
}

TEST_CASE("QueryExecutor JSON column", "[query_executor][json]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);

    QueryExecutor executor(database);
    executor.execute("INSTALL json; LOAD json;");

    SECTION("nested JSON value is embedded, not escaped") {
        // Reproduction from issue #38: a column with the DuckDB `JSON`
        // logical-type alias must serialise as nested JSON, not as a
        // JSON-escaped string the caller has to parse a second time.
        executor.execute(R"SQL(
            SELECT
                1 AS id,
                '{"a": 1, "b": [10, 20], "c": {"nested": true}}'::JSON AS payload
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["id"].i() == 1);

        // The crux: payload must be an Object, not a String.
        REQUIRE(doc[0]["payload"].t() == crow::json::type::Object);
        REQUIRE(doc[0]["payload"]["a"].i() == 1);
        REQUIRE(doc[0]["payload"]["b"].t() == crow::json::type::List);
        REQUIRE(doc[0]["payload"]["b"].size() == 2);
        REQUIRE(doc[0]["payload"]["b"][0].i() == 10);
        REQUIRE(doc[0]["payload"]["b"][1].i() == 20);
        REQUIRE(doc[0]["payload"]["c"]["nested"].b() == true);
    }

    SECTION("JSON array column is embedded as array") {
        executor.execute(R"SQL(
            SELECT '[1, 2, 3]'::JSON AS arr
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["arr"].t() == crow::json::type::List);
        REQUIRE(doc[0]["arr"].size() == 3);
        REQUIRE(doc[0]["arr"][0].i() == 1);
        REQUIRE(doc[0]["arr"][2].i() == 3);
    }

    SECTION("NULL JSON column stays null") {
        executor.execute(R"SQL(
            SELECT CAST(NULL AS JSON) AS payload
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["payload"].t() == crow::json::type::Null);
    }

    SECTION("plain VARCHAR is still emitted as a string") {
        // Regression guard: only the JSON-aliased VARCHAR path changes;
        // bare VARCHAR must continue to render as a JSON string.
        executor.execute(R"SQL(
            SELECT '{"looks":"like json"}'::VARCHAR AS not_json
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["not_json"].t() == crow::json::type::String);
        REQUIRE(doc[0]["not_json"].s() == "{\"looks\":\"like json\"}");
    }

    duckdb_close(&database);
}

TEST_CASE("QueryExecutor type coverage", "[query_executor]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    
    QueryExecutor executor(database);
    executor.execute(R"(
        SELECT 
            1::TINYINT as tiny,
            2::SMALLINT as small,
            3::INTEGER as integer,
            4::BIGINT as big,
            5.5::FLOAT as float,
            6.6::DOUBLE as double,
            '2023-01-01'::DATE as date,
            '12:34:56'::TIME as time,
            '2023-01-01 12:34:56'::TIMESTAMP as timestamp,
            '2023-01-01 12:34:56'::TIMESTAMPTZ as timestamp_tz,
            {'key': 'value'} as struct,
            [1,2,3] as list,
            TRUE as boolean,
            INTERVAL 1 MONTH as interval,
            'hello'::VARCHAR as varchar,
            '123.456'::DECIMAL(6, 3) as decimal
    )");

    auto el = executor.toJson()[0];
    auto doc = crow::json::load(el.dump());

    std::cout << "Type doc: " << doc << std::endl;
    
    REQUIRE(doc["tiny"].i() == 1);
    REQUIRE(doc["small"].i() == 2);
    REQUIRE(doc["integer"].i() == 3);
    REQUIRE(doc["big"].i() == 4);
    REQUIRE(doc["float"].d() == Catch::Approx(5.5));
    REQUIRE(doc["double"].d() == Catch::Approx(6.6));
    REQUIRE(doc["date"].s() == "2023-01-01");
    REQUIRE(doc["time"].s() == "12:34:56.000");
    REQUIRE(std::string(doc["timestamp"].s()).find("2023-01-01T12:34:56") != std::string::npos);
    REQUIRE(std::string(doc["timestamp_tz"].s()).find("2023-01-01T12:34:56") != std::string::npos);
    REQUIRE(doc["struct"]["key"].s() == "value");
    REQUIRE(doc["list"].size() == 3);
    REQUIRE(doc["boolean"].b() == true);
    REQUIRE(doc["interval"].s() == "01:00.000");
    REQUIRE(doc["varchar"].s() == "hello");
    REQUIRE(doc["decimal"].d() == Catch::Approx(123.456));

    duckdb_close(&database);
}

TEST_CASE("QueryExecutor chunk experiment", "[query_executor]") {
    duckdb_database database;
	REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
	
    QueryExecutor executor(database);
    executor.execute(R"SQL(
        select {'a': 42::int} as a, 
               {'b': 'hello', 'c': [1, 2, 3]} as b, 
               {'d': {'e': 42}} as c, 
               ['2025-01-01 12:00:00'::timestamp, '2025-01-02 12:00:00'::timestamp] as d,
               3::int as e
    )SQL");

    auto doc = crow::json::load(executor.toJson().dump());
    std::cout << "************" << std::endl;
    std::cout << doc << std::endl;
    std::cout << "************" << std::endl;

    REQUIRE(doc.size() == 1);
    REQUIRE(doc[0]["a"]["a"].i() == 42);
    REQUIRE(doc[0]["b"]["b"].s() == "hello");
    REQUIRE(doc[0]["b"]["c"].size() == 3);
    REQUIRE(doc[0]["c"]["d"]["e"].i() == 42);
    REQUIRE(doc[0]["d"].size() == 2);
    REQUIRE(doc[0]["e"].i() == 3);

	duckdb_close(&database);
}

TEST_CASE("QueryExecutor LIST/STRUCT per-row serialization", "[query_executor][list]") {
    // Regression for issue #89: native LIST(STRUCT) / LIST(VARCHAR) columns
    // must serialize each row's own slice of the list child vector, not the
    // whole child vector, and structs nested inside a list must read their
    // own element rather than always element[0].
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);

    SECTION("single-row LIST(STRUCT) emits distinct elements") {
        QueryExecutor executor(database);
        executor.execute(R"SQL(
            SELECT [
                {'stage': 'prepare',     'n_dropped': 11945},
                {'stage': 'features',    'n_dropped': 57203},
                {'stage': 'postprocess', 'n_dropped': 238505}
            ] AS by_stage
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["by_stage"].size() == 3);
        REQUIRE(doc[0]["by_stage"][0]["stage"].s() == "prepare");
        REQUIRE(doc[0]["by_stage"][0]["n_dropped"].i() == 11945);
        REQUIRE(doc[0]["by_stage"][1]["stage"].s() == "features");
        REQUIRE(doc[0]["by_stage"][1]["n_dropped"].i() == 57203);
        REQUIRE(doc[0]["by_stage"][2]["stage"].s() == "postprocess");
        REQUIRE(doc[0]["by_stage"][2]["n_dropped"].i() == 238505);
    }

    SECTION("multi-row LIST(VARCHAR) keeps each row's own list") {
        QueryExecutor executor(database);
        executor.execute(R"SQL(
            SELECT * FROM (VALUES
                (1, ['a1', 'a2', 'a3']),
                (2, ['b1', 'b2', 'b3']),
                (3, ['c1', 'c2', 'c3']),
                (4, ['d1', 'd2', 'd3'])
            ) AS t(id, flags)
            ORDER BY id
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 4);

        REQUIRE(doc[0]["flags"].size() == 3);
        REQUIRE(doc[0]["flags"][0].s() == "a1");
        REQUIRE(doc[0]["flags"][2].s() == "a3");

        REQUIRE(doc[1]["flags"].size() == 3);
        REQUIRE(doc[1]["flags"][0].s() == "b1");
        REQUIRE(doc[1]["flags"][2].s() == "b3");

        REQUIRE(doc[3]["flags"].size() == 3);
        REQUIRE(doc[3]["flags"][0].s() == "d1");
        REQUIRE(doc[3]["flags"][2].s() == "d3");
    }

    SECTION("multi-row LIST(STRUCT) keeps each row's own elements") {
        QueryExecutor executor(database);
        executor.execute(R"SQL(
            SELECT * FROM (VALUES
                (1, [{'k': 'a', 'v': 1}, {'k': 'b', 'v': 2}]),
                (2, [{'k': 'c', 'v': 3}, {'k': 'd', 'v': 4}])
            ) AS t(id, items)
            ORDER BY id
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 2);

        REQUIRE(doc[0]["items"].size() == 2);
        REQUIRE(doc[0]["items"][0]["k"].s() == "a");
        REQUIRE(doc[0]["items"][0]["v"].i() == 1);
        REQUIRE(doc[0]["items"][1]["k"].s() == "b");
        REQUIRE(doc[0]["items"][1]["v"].i() == 2);

        REQUIRE(doc[1]["items"].size() == 2);
        REQUIRE(doc[1]["items"][0]["k"].s() == "c");
        REQUIRE(doc[1]["items"][0]["v"].i() == 3);
        REQUIRE(doc[1]["items"][1]["k"].s() == "d");
        REQUIRE(doc[1]["items"][1]["v"].i() == 4);
    }

    SECTION("multi-row STRUCT column reads its own row") {
        QueryExecutor executor(database);
        executor.execute(R"SQL(
            SELECT * FROM (VALUES
                (1, {'name': 'alice', 'age': 30}),
                (2, {'name': 'bob',   'age': 40})
            ) AS t(id, person)
            ORDER BY id
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 2);
        REQUIRE(doc[0]["person"]["name"].s() == "alice");
        REQUIRE(doc[0]["person"]["age"].i() == 30);
        REQUIRE(doc[1]["person"]["name"].s() == "bob");
        REQUIRE(doc[1]["person"]["age"].i() == 40);
    }

    SECTION("NULL list entry stays null") {
        QueryExecutor executor(database);
        executor.execute(R"SQL(
            SELECT * FROM (VALUES
                (1, ['x', 'y']),
                (2, CAST(NULL AS VARCHAR[]))
            ) AS t(id, flags)
            ORDER BY id
        )SQL");

        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 2);
        REQUIRE(doc[0]["flags"].size() == 2);
        REQUIRE(doc[1]["flags"].t() == crow::json::type::Null);
    }

    duckdb_close(&database);
}

TEST_CASE("QueryExecutor::executeWithBindings - prepared path", "[query_executor][prepared]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);

    SECTION("bind int64 + varchar + null mix") {
        QueryExecutor executor(database);
        // Seed a table to exercise typed binding end-to-end.
        executor.execute("CREATE TABLE t (id BIGINT, name VARCHAR, score DOUBLE)");
        executor.execute("INSERT INTO t VALUES (1,'alice',1.5),(2,'bob',2.5),(3,NULL,3.5)");

        std::vector<PreparedBindingSpec> b = {
            {"id",   SqlParameterType::Integer, 0},
            {"name", SqlParameterType::Varchar, 1},
        };
        std::map<std::string, std::string> v = {{"id", "2"}, {"name", "bob"}};

        executor.executeWithBindings(
            "SELECT id, name FROM t WHERE id = ? AND name = ?",
            b, v, "test");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["id"].i() == 2);
        REQUIRE(doc[0]["name"].s() == "bob");
    }

    SECTION("missing param binds NULL and matches IS NULL") {
        QueryExecutor executor(database);
        executor.execute("CREATE TABLE u (id BIGINT, opt VARCHAR)");
        executor.execute("INSERT INTO u VALUES (10,NULL),(11,'x')");

        std::vector<PreparedBindingSpec> b = {
            {"opt", SqlParameterType::Varchar, 0},
        };
        std::map<std::string, std::string> v;  // 'opt' absent → NULL

        // `IS NOT DISTINCT FROM` returns true when both sides are NULL.
        executor.executeWithBindings(
            "SELECT id FROM u WHERE opt IS NOT DISTINCT FROM ? ORDER BY id",
            b, v, "test");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["id"].i() == 10);
    }

    SECTION("SQL injection payload bound as data, not parsed") {
        QueryExecutor executor(database);
        executor.execute("CREATE TABLE s (id BIGINT, label VARCHAR)");
        executor.execute("INSERT INTO s VALUES (1,'normal')");

        std::vector<PreparedBindingSpec> b = {
            {"label", SqlParameterType::Varchar, 0},
        };
        // Classic injection payload — must end up as a literal compared to
        // 'normal', not parsed.
        std::map<std::string, std::string> v = {
            {"label", "' OR 1=1 --"}
        };

        executor.executeWithBindings(
            "SELECT id FROM s WHERE label = ?", b, v, "test");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 0);
    }

    SECTION("bad numeric input surfaces as bind error") {
        QueryExecutor executor(database);
        executor.execute("CREATE TABLE n (id BIGINT)");

        std::vector<PreparedBindingSpec> b = {
            {"id", SqlParameterType::Integer, 0},
        };
        std::map<std::string, std::string> v = {{"id", "not-an-int"}};

        REQUIRE_THROWS_AS(
            executor.executeWithBindings("SELECT * FROM n WHERE id = ?", b, v, "test"),
            BadRequestError);
        REQUIRE_THROWS_WITH(
            executor.executeWithBindings("SELECT * FROM n WHERE id = ?", b, v, "test"),
            Catch::Matchers::ContainsSubstring("Invalid value for parameter"));
    }

    SECTION("date and time bindings round-trip") {
        QueryExecutor executor(database);
        executor.execute("CREATE TABLE dt (d DATE, t TIME)");
        executor.execute("INSERT INTO dt VALUES (DATE '2024-03-15', TIME '13:45:07')");

        std::vector<PreparedBindingSpec> b = {
            {"d", SqlParameterType::Date, 0},
            {"t", SqlParameterType::Time, 1},
        };
        std::map<std::string, std::string> v = {
            {"d", "2024-03-15"},
            {"t", "13:45:07"},
        };

        executor.executeWithBindings(
            "SELECT 1 AS hit FROM dt WHERE d = ? AND t = ?", b, v, "test");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc.size() == 1);
        REQUIRE(doc[0]["hit"].i() == 1);
    }

    SECTION("empty bindings vector behaves like execute()") {
        QueryExecutor executor(database);
        std::vector<PreparedBindingSpec> b;
        std::map<std::string, std::string> v;
        executor.executeWithBindings("SELECT 7 AS n", b, v, "test");
        auto doc = crow::json::load(executor.toJson().dump());
        REQUIRE(doc[0]["n"].i() == 7);
    }

    SECTION("prepare failure (syntax error) raises with diagnostic") {
        QueryExecutor executor(database);
        std::vector<PreparedBindingSpec> b = {{"id", SqlParameterType::Integer, 0}};
        std::map<std::string, std::string> v = {{"id", "1"}};
        REQUIRE_THROWS_WITH(
            executor.executeWithBindings("SELEKT ? FROM nowhere", b, v, "test"),
            Catch::Matchers::ContainsSubstring("Prepare failed"));
    }

    duckdb_close(&database);
}

}  // namespace test
}  // namespace flapi

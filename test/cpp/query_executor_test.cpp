#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
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

}
}
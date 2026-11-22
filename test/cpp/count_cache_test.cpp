#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "count_cache.hpp"

using namespace flapi;

namespace {

bool Countable(const std::string& sql, std::vector<std::string>* functions_out = nullptr) {
    std::vector<std::string> functions;
    const bool ok = CountableQuery(sql, "cache", functions);
    if (functions_out) {
        *functions_out = functions;
    }
    return ok;
}

}  // namespace

TEST_CASE("CountableQuery accepts SELECTs over DuckLake tables only", "[count_cache]") {
    CHECK(Countable("SELECT * FROM cache.main.t ORDER BY id"));
    CHECK(Countable("SELECT * FROM cache.main.t WHERE id >= ? ORDER BY id"));
    CHECK(Countable("SELECT * FROM \"CACHE\".main.t AT (VERSION => 3)"));
    CHECK(Countable("WITH x AS (SELECT * FROM cache.main.t) SELECT * FROM x JOIN cache.s.u USING (id)"));
    CHECK(Countable("SELECT * FROM cache.main.t WHERE id IN (SELECT id FROM cache.main.u)"));
    CHECK(Countable("SELECT a FROM cache.main.t UNION ALL SELECT a FROM cache.main.u"));

    // Anything that may resolve outside the catalog, or is not one SELECT.
    CHECK_FALSE(Countable("SELECT * FROM t"));
    CHECK_FALSE(Countable("SELECT * FROM main.t"));
    CHECK_FALSE(Countable("SELECT * FROM memory.main.t"));
    CHECK_FALSE(Countable("SELECT * FROM read_parquet('x.parquet')"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t JOIN read_csv('s.csv') USING (id)"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t WHERE id IN (SELECT id FROM other.main.u)"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t USING SAMPLE 10%"));
    CHECK_FALSE(Countable("INSERT INTO cache.main.t VALUES (1)"));
    CHECK_FALSE(Countable("SELECT 1; SELECT 2"));
    CHECK_FALSE(Countable("SELECT * FROM"));
}

TEST_CASE("CountableQuery reports every function for the stability check", "[count_cache]") {
    std::vector<std::string> functions;
    REQUIRE(Countable("SELECT lower(name) FROM cache.main.t WHERE now() > ts AND id + 1 > 2 "
                      "QUALIFY row_number() OVER () < 5",
                      &functions));
    for (const char* expected : {"lower", "now", "+", "row_number"}) {
        CHECK(std::find(functions.begin(), functions.end(), expected) != functions.end());
    }
    REQUIRE(Countable("SELECT * FROM cache.main.t AT (TIMESTAMP => now())", &functions));
    CHECK(std::find(functions.begin(), functions.end(), "now") != functions.end());
}

TEST_CASE("CountCache is exact per generation and bounded", "[count_cache]") {
    CountCache cache(2);
    const auto v1 = cache.storeVerdict("SELECT 1", true);
    const auto v2 = cache.storeVerdict("SELECT 2", false);
    CHECK(v1.id != v2.id);
    CHECK(cache.verdict("SELECT 1")->eligible);
    CHECK_FALSE(cache.verdict("SELECT 2")->eligible);
    CHECK_FALSE(cache.verdict("SELECT 3"));
    cache.store("a", 1, 10);
    CHECK(cache.find("a", 1) == 10);
    CHECK_FALSE(cache.find("a", 2));      // another generation: gone
    CHECK_FALSE(cache.find("a", 1));
    cache.store("a", 5, 1);
    cache.store("b", 5, 2);
    CHECK(cache.find("a", 5) == 1);       // a is now the most recent
    cache.store("c", 5, 3);               // evicts b
    CHECK_FALSE(cache.find("b", 5));
    CHECK(cache.find("a", 5) == 1);
    CHECK(cache.find("c", 5) == 3);
    CHECK(cache.size() == 2);
    cache.store("a", 4, 99);              // never back to an older generation
    CHECK(cache.find("a", 5) == 1);
}

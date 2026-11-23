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
    REQUIRE(Countable("SELECT lower(name) FROM cache.main.t WHERE abs(x) > 1 AND id + 1 > 2 "
                      "QUALIFY row_number() OVER () < 5",
                      &functions));
    for (const char* expected : {"lower", "abs", "+", "row_number"}) {
        CHECK(std::find(functions.begin(), functions.end(), expected) != functions.end());
    }
    REQUIRE(Countable("SELECT * FROM cache.main.t AT (TIMESTAMP => to_timestamp(5))", &functions));
    CHECK(std::find(functions.begin(), functions.end(), "to_timestamp") != functions.end());
}

TEST_CASE("CountableQuery refuses functions that read the clock, session or randomness", "[count_cache]") {
    // age(TIMESTAMP) reads the transaction start, yet DuckDB reports it CONSISTENT.
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t WHERE age(ts) < INTERVAL '1 year'"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t WHERE AGE(ts) < INTERVAL '1 year'"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t AT (TIMESTAMP => now())"));
    // Without parentheses these parse as column references and bind as the function.
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t WHERE d < current_date"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t WHERE ts < CURRENT_TIMESTAMP"));
    CHECK(Countable("SELECT * FROM cache.main.t WHERE t.current_date < DATE '2020-01-01'"));
    CHECK_FALSE(Countable("SELECT random() FROM cache.main.t"));
    CHECK_FALSE(Countable("SELECT json_serialize_plan('SELECT 1') FROM cache.main.t"));
    CHECK(UnstableFunctionName("age"));
    CHECK_FALSE(UnstableFunctionName("date_part"));
}

TEST_CASE("CountableQuery refuses samples and reports three-part tables", "[count_cache]") {
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t TABLESAMPLE 10%"));
    CHECK_FALSE(Countable("SELECT * FROM cache.main.t JOIN cache.main.u TABLESAMPLE 5 ROWS USING (id)"));
    std::vector<std::string> functions;
    std::vector<TableName> tables;
    REQUIRE(CountableQuery("WITH x AS (SELECT * FROM Cache.Main.T) SELECT * FROM x JOIN cache.s.\"V\" USING (id)",
                           "cache", functions, tables));
    REQUIRE(tables.size() == 2);
    CHECK(tables[0].catalog == "cache");
    CHECK(tables[0].schema == "main");
    CHECK(tables[0].name == "t");
    CHECK(tables[1].schema == "s");
    CHECK(tables[1].name == "v");
}

TEST_CASE("Count keys are length-prefixed", "[count_cache]") {
    const auto key = [](std::initializer_list<std::pair<std::string, std::string>> pairs) {
        std::string k;
        for (const auto& [name, value] : pairs) {
            AppendKeyPart(k, name);
            k += '=';
            AppendKeyPart(k, value);
        }
        return k;
    };
    // name=value\x1f joined these to the same text.
    CHECK(key({{"a", "p\x1f" "b=q"}, {"b", "r"}}) != key({{"a", "p"}, {"b", "q\x1f" "b=r"}}));
    CHECK(key({{"a", "1:x"}}) != key({{"a", "1"}, {"x", ""}}));
}

TEST_CASE("CountCacheSwitch turns off, retries after the backoff, and reports transitions", "[count_cache]") {
    CountCacheSwitch power;
    CHECK(power.usable(0));
    CHECK_FALSE(power.succeeded());                       // already on: nothing to log
    CHECK(power.failed(1000));                            // on -> off: logged
    CHECK_FALSE(power.usable(1000 + CountCacheSwitch::kRetryAfterMs - 1));
    CHECK(power.usable(1000 + CountCacheSwitch::kRetryAfterMs));   // try again
    CHECK_FALSE(power.failed(1000 + CountCacheSwitch::kRetryAfterMs));   // still off: not logged again
    CHECK_FALSE(power.usable(1000 + 2 * CountCacheSwitch::kRetryAfterMs - 1));
    CHECK(power.usable(1000 + 2 * CountCacheSwitch::kRetryAfterMs));
    CHECK(power.succeeded());                             // off -> on: logged
    CHECK(power.usable(0));
}

TEST_CASE("Tables verdicts are per generation", "[count_cache]") {
    CountCache cache;
    cache.storeVerdict("SELECT 1", true);
    CHECK_FALSE(cache.tablesVerdict("SELECT 1", 3));
    cache.storeTablesVerdict("SELECT 1", 3, true);
    CHECK(cache.tablesVerdict("SELECT 1", 3) == true);
    CHECK_FALSE(cache.tablesVerdict("SELECT 1", 4));      // a new generation: judged again
    cache.storeTablesVerdict("SELECT 1", 4, false);
    CHECK(cache.tablesVerdict("SELECT 1", 4) == false);
    cache.storeTablesVerdict("SELECT 1", 3, true);        // never back to an older generation
    CHECK(cache.tablesVerdict("SELECT 1", 4) == false);
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

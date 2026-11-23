#include "count_cache.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"

namespace flapi {

namespace {

// Verdicts are per distinct rendered SQL; a template whose text varies per
// request (a raw-substituted value) must not grow this without bound.
constexpr std::size_t kMaxVerdicts = 4096;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Walks one parsed SELECT and answers whether it reads DuckLake tables only.
class CountableWalker {
public:
    CountableWalker(std::string alias, std::vector<std::string>& functions, std::vector<TableName>& tables)
        : alias_(Lower(std::move(alias))), functions_(functions), tables_(tables) {}

    bool ok() const { return ok_; }

    void node(duckdb::QueryNode& n, std::set<std::string> ctes) {
        if (!ok_) {
            return;
        }
        for (auto& entry : n.cte_map.map) {
            ctes.insert(Lower(entry.first.GetIdentifierName()));
        }
        for (auto& entry : n.cte_map.map) {
            if (!entry.second || !entry.second->query_node) {
                ok_ = false;
                return;
            }
            node(*entry.second->query_node, ctes);
        }
        duckdb::ParsedExpressionIterator::EnumerateQueryNodeModifiers(
            n, [&](duckdb::unique_ptr<duckdb::ParsedExpression>& e) { expr(e.get(), ctes); });
        switch (n.type) {
            case duckdb::QueryNodeType::SELECT_NODE: {
                auto& s = n.Cast<duckdb::SelectNode>();
                for (auto& e : s.select_list) {
                    expr(e.get(), ctes);
                }
                for (auto& e : s.groups.group_expressions) {
                    expr(e.get(), ctes);
                }
                expr(s.where_clause.get(), ctes);
                expr(s.having.get(), ctes);
                expr(s.qualify.get(), ctes);
                if (s.sample) {
                    // A sample is random unless seeded; not worth judging.
                    ok_ = false;
                    return;
                }
                if (s.from_table) {
                    ref(*s.from_table, ctes);
                }
                return;
            }
            case duckdb::QueryNodeType::SET_OPERATION_NODE: {
                for (auto& child : n.Cast<duckdb::SetOperationNode>().children) {
                    node(*child, ctes);
                }
                return;
            }
            case duckdb::QueryNodeType::RECURSIVE_CTE_NODE: {
                auto& r = n.Cast<duckdb::RecursiveCTENode>();
                ctes.insert(Lower(r.ctename.GetIdentifierName()));
                node(*r.left, ctes);
                node(*r.right, ctes);
                return;
            }
            default:
                ok_ = false;
                return;
        }
    }

private:
    void ref(duckdb::TableRef& r, const std::set<std::string>& ctes) {
        if (!ok_) {
            return;
        }
        if (r.sample) {
            // TABLESAMPLE on a table reference: as random as USING SAMPLE.
            ok_ = false;
            return;
        }
        switch (r.type) {
            case duckdb::TableReferenceType::BASE_TABLE: {
                auto& base = r.Cast<duckdb::BaseTableRef>();
                const auto& name = base.GetQualifiedName();
                const auto& path = name.Path();
                if (path.size() == 1 && ctes.count(Lower(name.Name().GetIdentifierName())) > 0) {
                    return;   // a CTE of this query
                }
                // Fully qualified into the DuckLake catalog, nothing less: an
                // unqualified or two-part name may resolve to the main database.
                if (path.size() != 3 || Lower(name.Catalog().GetIdentifierName()) != alias_) {
                    ok_ = false;
                    return;
                }
                tables_.push_back(TableName{Lower(name.Catalog().GetIdentifierName()),
                                            Lower(name.Schema().GetIdentifierName()),
                                            Lower(name.Name().GetIdentifierName())});
                if (base.at_clause) {
                    expr(base.at_clause->Expression().get(), ctes);
                }
                return;
            }
            case duckdb::TableReferenceType::JOIN: {
                auto& j = r.Cast<duckdb::JoinRef>();
                ref(*j.left, ctes);
                ref(*j.right, ctes);
                expr(j.condition.get(), ctes);
                return;
            }
            case duckdb::TableReferenceType::SUBQUERY: {
                auto& sq = r.Cast<duckdb::SubqueryRef>();
                if (!sq.subquery || !sq.subquery->node) {
                    ok_ = false;
                    return;
                }
                node(*sq.subquery->node, ctes);
                return;
            }
            case duckdb::TableReferenceType::EXPRESSION_LIST: {
                for (auto& row : r.Cast<duckdb::ExpressionListRef>().values) {
                    for (auto& e : row) {
                        expr(e.get(), ctes);
                    }
                }
                return;
            }
            case duckdb::TableReferenceType::EMPTY_FROM:
                return;
            default:
                // Table functions (read_parquet, ducklake_snapshots, ...), PIVOT,
                // SHOW and anything newer: not provably catalog-only.
                ok_ = false;
                return;
        }
    }

    void expr(const duckdb::ParsedExpression* e, const std::set<std::string>& ctes) {
        if (!ok_ || e == nullptr) {
            return;
        }
        if (e->GetExpressionClass() == duckdb::ExpressionClass::FUNCTION) {
            const auto& f = e->Cast<duckdb::FunctionExpression>();
            functions_.push_back(Lower(f.FunctionName().GetIdentifierName()));
        } else if (e->GetExpressionClass() == duckdb::ExpressionClass::SUBQUERY) {
            const auto& sq = e->Cast<duckdb::SubqueryExpression>();
            if (!sq.Subquery() || !sq.Subquery()->node) {
                ok_ = false;
                return;
            }
            node(*sq.Subquery()->node, ctes);
        } else if (e->GetExpressionClass() == duckdb::ExpressionClass::COLUMN_REF) {
            // `current_date`, `current_timestamp`, `user`, ... without parentheses
            // parse as a column and bind as the function when no column has the
            // name: judged by name too (a real column so named just never caches).
            const auto& c = e->Cast<duckdb::ColumnRefExpression>();
            if (!c.IsQualified()) {
                const auto name = Lower(c.GetColumnName().GetIdentifierName());
                if (UnstableFunctionName(name)) {
                    functions_.push_back(name);
                }
            }
        } else if (e->GetExpressionClass() == duckdb::ExpressionClass::WINDOW) {
            // Window functions are judged by name like any other.
            functions_.push_back(Lower(e->Cast<duckdb::WindowExpression>().FunctionName().GetIdentifierName()));
        }
        duckdb::ParsedExpressionIterator::EnumerateChildren(
            *e, [&](const duckdb::ParsedExpression& child) { expr(&child, ctes); });
    }

    std::string alias_;
    std::vector<std::string>& functions_;
    std::vector<TableName>& tables_;
    bool ok_ = true;
};

}  // namespace

// Audited in the vendored DuckDB (2.0.1) for functions that read the clock,
// the transaction, the session or a random source while duckdb_functions()
// lets them through (no VOLATILE overload, not every overload
// CONSISTENT_WITHIN_QUERY):
//   * age(TIMESTAMP) - core_functions/scalar/date/age.cpp reads
//     MetaTransaction::start_timestamp yet is CONSISTENT; the ICU TIMESTAMPTZ
//     overload (CurrentMidnight) is CONSISTENT_WITHIN_QUERY, so the name is mixed;
//   * json_serialize_plan - binds its argument against the live catalogs of
//     every attached database, not only DuckLake.
// Every other mixed name is a TIMESTAMPTZ overload that reads the TimeZone /
// Calendar setting (fixed for the server). The rest of the list is already
// refused by its stability and is named here so that a future DuckDB that
// relabels one cannot slip it through.
bool UnstableFunctionName(const std::string& lower_name) {
    static const std::set<std::string> kNames = {
        "age", "json_serialize_plan",
        // clock and transaction
        "now", "today", "current_date", "current_time", "current_timestamp", "get_current_time",
        "get_current_timestamp", "transaction_timestamp", "localtime", "localtimestamp", "txid_current",
        // session
        "current_setting", "getvariable", "current_query", "current_schema", "current_schemas",
        "current_database", "current_catalog", "current_user", "current_role", "session_user", "user",
        "in_search_path",
        // random and sequences
        "random", "setseed", "uuid", "gen_random_uuid", "uuidv4", "uuidv7", "nextval", "currval",
    };
    return kNames.count(lower_name) > 0;
}

void AppendKeyPart(std::string& key, std::string_view part) {
    key += std::to_string(part.size());
    key += ':';
    key.append(part.data(), part.size());
}

bool CountCacheSwitch::usable(std::int64_t now_ms) const {
    return !off_.load(std::memory_order_relaxed) || now_ms >= retry_at_ms_.load(std::memory_order_relaxed);
}

bool CountCacheSwitch::failed(std::int64_t now_ms) {
    retry_at_ms_.store(now_ms + kRetryAfterMs, std::memory_order_relaxed);
    return !off_.exchange(true);
}

bool CountCacheSwitch::succeeded() {
    if (!off_.load(std::memory_order_relaxed)) {
        return false;
    }
    return off_.exchange(false);
}

CountCache::CountCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

std::optional<std::int64_t> CountCache::find(const std::string& key, std::int64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    if (it->second.generation != generation) {
        lru_.erase(it->second.lru);
        entries_.erase(it);
        return std::nullopt;
    }
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.count;
}

void CountCache::store(const std::string& key, std::int64_t generation, std::int64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        // Never move an entry back to an older generation.
        if (generation >= it->second.generation) {
            it->second.generation = generation;
            it->second.count = count;
        }
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return;
    }
    while (entries_.size() >= capacity_ && !lru_.empty()) {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_front(key);
    entries_.emplace(key, Entry{generation, count, lru_.begin()});
}

std::optional<CountCache::Verdict> CountCache::verdict(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = verdicts_.find(sql);
    if (it == verdicts_.end()) {
        return std::nullopt;
    }
    return it->second.verdict;
}

std::optional<bool> CountCache::tablesVerdict(const std::string& sql, std::int64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = verdicts_.find(sql);
    if (it == verdicts_.end() || it->second.tables_generation != generation) {
        return std::nullopt;
    }
    return it->second.tables_ok;
}

void CountCache::storeTablesVerdict(const std::string& sql, std::int64_t generation, bool tables_ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = verdicts_.find(sql);
    if (it == verdicts_.end() || generation < it->second.tables_generation) {
        return;
    }
    it->second.tables_generation = generation;
    it->second.tables_ok = tables_ok;
}

CountCache::Verdict CountCache::storeVerdict(const std::string& sql, bool eligible) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = verdicts_.find(sql);
    if (it != verdicts_.end()) {
        return it->second.verdict;   // judged concurrently: keep the first id
    }
    if (verdicts_.size() >= kMaxVerdicts) {
        // Counts keyed by a forgotten id are never looked up again and age out.
        verdicts_.clear();
    }
    const Verdict v{eligible, next_verdict_id_++};
    verdicts_.emplace(sql, StoredVerdict{v});
    return v;
}

std::size_t CountCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void CountCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
    verdicts_.clear();
}

bool CountableQuery(const std::string& sql, const std::string& ducklake_alias,
                    std::vector<std::string>& functions) {
    std::vector<TableName> tables;
    return CountableQuery(sql, ducklake_alias, functions, tables);
}

bool CountableQuery(const std::string& sql, const std::string& ducklake_alias,
                    std::vector<std::string>& functions, std::vector<TableName>& tables) {
    if (ducklake_alias.empty()) {
        return false;
    }
    try {
        auto parser = duckdb::Parser::GetBuiltinParser();
        parser.ParseQuery(sql);
        if (parser.statements.size() != 1 || !parser.statements[0] ||
            parser.statements[0]->type != duckdb::StatementType::SELECT_STATEMENT) {
            return false;
        }
        auto& select = parser.statements[0]->Cast<duckdb::SelectStatement>();
        if (!select.node) {
            return false;
        }
        CountableWalker walker(ducklake_alias, functions, tables);
        walker.node(*select.node, {});
        if (!walker.ok()) {
            return false;
        }
        return std::none_of(functions.begin(), functions.end(), UnstableFunctionName);
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace flapi

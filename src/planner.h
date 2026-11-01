#ifndef PLANNER_H
#define PLANNER_H

#include "operator.h"
#include "ops_basic.h"
#include "sql_ast.h"
#include "expr_eval.h"

#include <memory>
#include <vector>
#include <string>
#include <deque>

struct Plan {
    std::unique_ptr<Operator> root;
    std::deque<std::string> literal_arena; // owns all string literals in expressions
    std::vector<std::string> output_headers;
    std::vector<size_t> needed_columns;
    std::vector<std::string> needed_column_names;
    std::string table_name;
    std::string csv_path;
    std::string where_sql;
    std::vector<std::string> select_sqls;
    long long limit = -1;
    long long offset = 0;
    bool has_limit = false;
    bool has_filter = false;

    // Aggregate query info
    bool has_aggregate = false;
    std::vector<std::string> group_by_sqls;
    std::vector<std::string> aggregate_sqls;
    bool has_having = false;
    std::string having_sql;

    // Distinct info
    bool has_distinct = false;

    // Sort info
    bool has_sort = false;
    struct SortTermPlan {
        std::string sql;
        bool descending = false;
    };
    std::vector<SortTermPlan> sort_terms;
    bool sort_is_top_n = false;
    long long sort_top_n = 0;

    // Output projection dimensions
    size_t visible_width = 0;
    size_t hidden_columns_count = 0;

    // Join info for explain
    struct JoinPlanInfo {
        std::string join_type;
        std::string probe_side;
        std::string build_side;
        bool swapped = false;
        bool is_nested_loop = false;
        std::vector<std::pair<std::string, std::string>> equi_keys;
        std::string residual_sql;
    };
    std::vector<JoinPlanInfo> join_plans;

    void explain(FILE* out) const;
};

struct PlanOptions {
    size_t max_groups = 0;
    size_t memory_limit_mb = 512;
    std::string tmp_dir = "/tmp";
    bool sort_stats = false;
    size_t sort_fan_in = 64;
    bool no_pushdown = false;
    bool join_stats = false;
};

// Check if a table scan can legally receive pushed-down filters
bool isTablePushdownLegal(size_t table_idx, const std::vector<JoinClause>& joins);

// Build an executable execution plan from a parsed SelectStmt
Plan buildPlan(const SelectStmt& stmt, const std::vector<std::string>& table_args, const PlanOptions& options);

// Backwards-compatible overloads
inline Plan buildPlan(const SelectStmt& stmt, const std::string& table_arg, const PlanOptions& options) {
    std::vector<std::string> args = {table_arg};
    return buildPlan(stmt, args, options);
}

inline Plan buildPlan(const SelectStmt& stmt, const std::string& table_arg, size_t max_groups = 0) {
    PlanOptions options;
    options.max_groups = max_groups;
    std::vector<std::string> args = {table_arg};
    return buildPlan(stmt, args, options);
}

#endif // PLANNER_H

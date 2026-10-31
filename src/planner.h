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

    void explain(FILE* out) const;
};

// Build an executable execution plan from a parsed SelectStmt
Plan buildPlan(const SelectStmt& stmt, const std::string& table_arg);

#endif // PLANNER_H

#include "planner.h"

#include <iostream>
#include <algorithm>
#include <cstdlib>

static inline std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) {
        c = asciiToLower(c);
    }
    return lower;
}

static void collectColumnIndices(
    const Expr* expr,
    const TableRef& from_table,
    const std::vector<Column>& schema,
    std::vector<size_t>& out_indices
) {
    if (!expr) return;

    if (expr->kind == ExprKind::COLUMN_REF) {
        if (!expr->table_name.empty()) {
            std::string prefix_lower = toLower(expr->table_name);
            std::string table_lower = toLower(from_table.name);
            std::string alias_lower = toLower(from_table.alias);
            if (prefix_lower != table_lower && (alias_lower.empty() || prefix_lower != alias_lower)) {
                std::cerr << "Error: unknown table qualifier '" << expr->table_name << "'\n";
                std::exit(2);
            }
        }
        std::string col_lower = toLower(expr->col_name);
        int found_idx = -1;
        for (size_t c = 0; c < schema.size(); ++c) {
            if (toLower(schema[c].name) == col_lower) {
                found_idx = static_cast<int>(c);
                break;
            }
        }
        if (found_idx < 0) {
            std::cerr << "Error: unknown column '" << expr->col_name << "'\n";
            std::exit(2);
        }
        out_indices.push_back(static_cast<size_t>(found_idx));
        return;
    }

    if (expr->kind == ExprKind::FUNCTION_CALL) {
        std::cerr << "not implemented yet: aggregate function: " << expr->func_name << "\n";
        std::exit(3);
    }

    collectColumnIndices(expr->child.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->left.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->right.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->between_expr.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->between_low.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->between_high.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->in_expr.get(), from_table, schema, out_indices);
    for (const auto& item : expr->in_list) {
        collectColumnIndices(item.get(), from_table, schema, out_indices);
    }
    collectColumnIndices(expr->like_expr.get(), from_table, schema, out_indices);
    collectColumnIndices(expr->like_pattern.get(), from_table, schema, out_indices);
}

Plan buildPlan(const SelectStmt& stmt, const std::string& table_arg) {
    Plan plan;

    // Parse table_arg
    plan.table_name = "t";
    plan.csv_path = table_arg;
    size_t eq_pos = table_arg.find('=');
    if (eq_pos != std::string::npos) {
        plan.table_name = table_arg.substr(0, eq_pos);
        plan.csv_path = table_arg.substr(eq_pos + 1);
    }

    // Check table name match (case-insensitive)
    std::string expected_lower = toLower(plan.table_name);
    std::string from_lower = toLower(stmt.from_table.name);
    if (from_lower != expected_lower) {
        std::cerr << "Error: unknown table '" << stmt.from_table.name << "'\n";
        std::exit(2);
    }

    // Check for unsupported features (exit code 3)
    if (stmt.distinct) {
        std::cerr << "not implemented yet: DISTINCT\n";
        std::exit(3);
    }
    if (!stmt.joins.empty()) {
        std::cerr << "not implemented yet: JOIN\n";
        std::exit(3);
    }
    if (!stmt.group_by.empty()) {
        std::cerr << "not implemented yet: GROUP BY\n";
        std::exit(3);
    }
    if (stmt.having) {
        std::cerr << "not implemented yet: HAVING\n";
        std::exit(3);
    }
    if (!stmt.order_by.empty()) {
        std::cerr << "not implemented yet: ORDER BY\n";
        std::exit(3);
    }

    // Open CSV to inspect schema
    CsvScanner scanner;
    if (!scanner.open(plan.csv_path.c_str())) {
        std::cerr << "Error: cannot open or read file '" << plan.csv_path << "'\n";
        std::exit(2);
    }
    std::vector<Column> schema = scanner.columns();
    scanner.close();

    // Expand SELECT items (* and t.*) and establish output headers
    struct ExpandedItem {
        std::unique_ptr<Expr> expr;
        std::string header;
        std::string sql;
    };
    std::vector<ExpandedItem> expanded_items;

    for (const auto& item : stmt.items) {
        if (item.expr->kind == ExprKind::STAR) {
            if (!item.expr->table_name.empty()) {
                std::string prefix_lower = toLower(item.expr->table_name);
                std::string alias_lower = toLower(stmt.from_table.alias);
                if (prefix_lower != expected_lower && (alias_lower.empty() || prefix_lower != alias_lower)) {
                    std::cerr << "Error: unknown table qualifier '" << item.expr->table_name << "'\n";
                    std::exit(2);
                }
            }
            for (const auto& col : schema) {
                auto col_expr = std::make_unique<Expr>();
                col_expr->kind = ExprKind::COLUMN_REF;
                col_expr->col_name = col.name;
                expanded_items.push_back({std::move(col_expr), col.name, col.name});
            }
        } else {
            std::string header;
            if (!item.alias.empty()) {
                header = item.alias;
            } else if (item.expr->kind == ExprKind::COLUMN_REF) {
                // Use file's column name for plain references
                std::string col_lower = toLower(item.expr->col_name);
                int found_idx = -1;
                for (size_t c = 0; c < schema.size(); ++c) {
                    if (toLower(schema[c].name) == col_lower) {
                        found_idx = static_cast<int>(c);
                        break;
                    }
                }
                if (found_idx < 0) {
                    std::cerr << "Error: unknown column '" << item.expr->col_name << "'\n";
                    std::exit(2);
                }
                header = schema[found_idx].name;
            } else {
                header = item.expr->toSql();
            }

            // Clone expr by printing toSql or moving
            // Since stmt is const, we can parse or create a clone, or bind directly
            // SqlParser can re-parse the expr toSql, or we can copy
            // Let's copy by re-parsing item.expr->toSql() or bind from item.expr directly
            expanded_items.push_back({nullptr, header, item.expr->toSql()});
        }
    }

    // Collect needed column indices from WHERE clause and all SELECT items
    std::vector<size_t> needed_cols;
    if (stmt.where_clause) {
        collectColumnIndices(stmt.where_clause.get(), stmt.from_table, schema, needed_cols);
        plan.has_filter = true;
        plan.where_sql = stmt.where_clause->toSql();
    }
    for (const auto& item : stmt.items) {
        if (item.expr->kind == ExprKind::STAR) {
            for (size_t c = 0; c < schema.size(); ++c) {
                needed_cols.push_back(c);
            }
        } else {
            collectColumnIndices(item.expr.get(), stmt.from_table, schema, needed_cols);
        }
    }

    std::sort(needed_cols.begin(), needed_cols.end());
    needed_cols.erase(std::unique(needed_cols.begin(), needed_cols.end()), needed_cols.end());

    plan.needed_columns = needed_cols;
    for (size_t c : needed_cols) {
        plan.needed_column_names.push_back(schema[c].name);
    }

    // Bind WHERE condition
    std::unique_ptr<BoundExpr> bound_where;
    if (stmt.where_clause) {
        bound_where = bindExpr(stmt.where_clause.get(), stmt.from_table, schema, plan.literal_arena);
    }

    // Bind SELECT expressions
    std::vector<std::unique_ptr<BoundExpr>> bound_selects;
    size_t exp_idx = 0;
    for (const auto& item : stmt.items) {
        if (item.expr->kind == ExprKind::STAR) {
            for (size_t c = 0; c < schema.size(); ++c) {
                auto b_col = std::make_unique<BoundExpr>();
                b_col->kind = BoundExprKind::COLUMN_REF;
                b_col->col_idx = c;
                b_col->col_name = schema[c].name;
                b_col->affinity = (schema[c].type == TYPE_STRING) ? Affinity::TEXT : Affinity::NUMERIC;
                bound_selects.push_back(std::move(b_col));
                plan.output_headers.push_back(expanded_items[exp_idx++].header);
                plan.select_sqls.push_back(schema[c].name);
            }
        } else {
            auto b_expr = bindExpr(item.expr.get(), stmt.from_table, schema, plan.literal_arena);
            bound_selects.push_back(std::move(b_expr));
            plan.output_headers.push_back(expanded_items[exp_idx++].header);
            plan.select_sqls.push_back(item.expr->toSql());
        }
    }

    // Extract table column types and names
    std::vector<ColType> col_types;
    std::vector<std::string> all_col_names;
    for (const auto& col : schema) {
        col_types.push_back(col.type);
        all_col_names.push_back(col.name);
    }

    // Build operator chain: Scan -> [Filter] -> Project -> [Limit]
    std::unique_ptr<Operator> op = std::make_unique<ScanOperator>(
        plan.csv_path,
        plan.needed_columns,
        col_types,
        all_col_names
    );

    if (bound_where) {
        op = std::make_unique<FilterOperator>(std::move(op), std::move(bound_where));
    }

    op = std::make_unique<ProjectOperator>(
        std::move(op),
        std::move(bound_selects),
        plan.output_headers
    );

    if (stmt.limit.has_value() || stmt.offset.has_value()) {
        plan.has_limit = true;
        plan.limit = stmt.limit.value_or(-1);
        plan.offset = stmt.offset.value_or(0);
        op = std::make_unique<LimitOperator>(std::move(op), plan.limit, plan.offset);
    }

    plan.root = std::move(op);
    return plan;
}

void Plan::explain(FILE* out) const {
    std::fprintf(out, "Scan (table: %s, file: %s, needed_columns: [", table_name.c_str(), csv_path.c_str());
    for (size_t i = 0; i < needed_column_names.size(); ++i) {
        if (i > 0) std::fprintf(out, ", ");
        std::fprintf(out, "%s", needed_column_names[i].c_str());
    }
    std::fprintf(out, "])\n");

    if (has_filter) {
        std::fprintf(out, "  Filter (where: %s)\n", where_sql.c_str());
    }

    std::fprintf(out, "    Project (columns: [");
    for (size_t i = 0; i < select_sqls.size(); ++i) {
        if (i > 0) std::fprintf(out, ", ");
        std::fprintf(out, "%s", select_sqls[i].c_str());
        if (output_headers[i] != select_sqls[i]) {
            std::fprintf(out, " AS %s", output_headers[i].c_str());
        }
    }
    std::fprintf(out, "])\n");

    if (has_limit) {
        std::fprintf(out, "      Limit (limit: %lld, offset: %lld)\n", limit, offset);
    }
}

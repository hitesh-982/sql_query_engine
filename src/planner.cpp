#include "planner.h"
#include "ops_agg.h"

#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <functional>

static inline std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) {
        c = asciiToLower(c);
    }
    return lower;
}

static bool isAggregateFunction(const std::string& name) {
    std::string l = toLower(name);
    return (l == "count" || l == "sum" || l == "avg" || l == "min" || l == "max");
}

static bool hasAggregateCall(const Expr* expr) {
    if (!expr) return false;
    if (expr->kind == ExprKind::FUNCTION_CALL && isAggregateFunction(expr->func_name)) {
        return true;
    }
    if (hasAggregateCall(expr->child.get())) return true;
    if (hasAggregateCall(expr->left.get())) return true;
    if (hasAggregateCall(expr->right.get())) return true;
    if (hasAggregateCall(expr->between_expr.get())) return true;
    if (hasAggregateCall(expr->between_low.get())) return true;
    if (hasAggregateCall(expr->between_high.get())) return true;
    if (hasAggregateCall(expr->in_expr.get())) return true;
    for (const auto& item : expr->in_list) {
        if (hasAggregateCall(item.get())) return true;
    }
    if (hasAggregateCall(expr->like_expr.get())) return true;
    if (hasAggregateCall(expr->like_pattern.get())) return true;
    for (const auto& arg : expr->func_args) {
        if (hasAggregateCall(arg.get())) return true;
    }
    return false;
}

static void collectAstColumnIndices(
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
        for (const auto& arg : expr->func_args) {
            collectAstColumnIndices(arg.get(), from_table, schema, out_indices);
        }
        return;
    }

    collectAstColumnIndices(expr->child.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->left.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->right.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->between_expr.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->between_low.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->between_high.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->in_expr.get(), from_table, schema, out_indices);
    for (const auto& item : expr->in_list) {
        collectAstColumnIndices(item.get(), from_table, schema, out_indices);
    }
    collectAstColumnIndices(expr->like_expr.get(), from_table, schema, out_indices);
    collectAstColumnIndices(expr->like_pattern.get(), from_table, schema, out_indices);
}

static void collectBoundColumnIndices(const BoundExpr* expr, std::vector<size_t>& out_indices) {
    if (!expr) return;
    if (expr->kind == BoundExprKind::COLUMN_REF) {
        out_indices.push_back(expr->col_idx);
        return;
    }
    collectBoundColumnIndices(expr->left.get(), out_indices);
    collectBoundColumnIndices(expr->right.get(), out_indices);
    collectBoundColumnIndices(expr->between_low.get(), out_indices);
    collectBoundColumnIndices(expr->between_high.get(), out_indices);
    for (const auto& item : expr->in_list) {
        collectBoundColumnIndices(item.get(), out_indices);
    }
    collectBoundColumnIndices(expr->like_pattern.get(), out_indices);
}

Plan buildPlan(const SelectStmt& stmt, const std::string& table_arg, size_t max_groups) {
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
    if (!stmt.order_by.empty()) {
        std::cerr << "not implemented yet: ORDER BY\n";
        std::exit(3);
    }

    // Validate WHERE clause: no aggregate functions allowed
    if (stmt.where_clause && hasAggregateCall(stmt.where_clause.get())) {
        std::cerr << "Error: misuse of aggregate function in WHERE clause\n";
        std::exit(2);
    }

    // Validate GROUP BY clause: no ordinals, no aggregate functions
    for (const auto& gb : stmt.group_by) {
        if (gb->kind == ExprKind::LITERAL_INT) {
            std::cerr << "not implemented yet: GROUP BY ordinal\n";
            std::exit(3);
        }
        if (hasAggregateCall(gb.get())) {
            std::cerr << "Error: aggregate functions are not allowed in GROUP BY\n";
            std::exit(2);
        }
    }

    // Open CSV to inspect schema
    CsvScanner scanner;
    if (!scanner.open(plan.csv_path.c_str())) {
        std::cerr << "Error: cannot open or read file '" << plan.csv_path << "'\n";
        std::exit(2);
    }
    std::vector<Column> schema = scanner.columns();
    scanner.close();

    // Check if this is an aggregate query
    bool is_aggregate_query = !stmt.group_by.empty();
    if (!is_aggregate_query) {
        for (const auto& item : stmt.items) {
            if (hasAggregateCall(item.expr.get())) {
                is_aggregate_query = true;
                break;
            }
        }
    }
    if (!is_aggregate_query && stmt.having) {
        if (hasAggregateCall(stmt.having.get())) {
            is_aggregate_query = true;
        } else {
            // HAVING on non-aggregate query without aggregates is not supported
            std::cerr << "not implemented yet: HAVING clause on a non-aggregate query\n";
            std::exit(3);
        }
    }

    // Extract table column types and names
    std::vector<ColType> col_types;
    std::vector<std::string> all_col_names;
    for (const auto& col : schema) {
        col_types.push_back(col.type);
        all_col_names.push_back(col.name);
    }

    if (!is_aggregate_query) {
        // ---------------------------------------------------------------------
        // Phase 2 path: Non-aggregate query
        // ---------------------------------------------------------------------
        if (stmt.having) {
            std::cerr << "not implemented yet: HAVING clause on a non-aggregate query\n";
            std::exit(3);
        }

        struct ExpandedItem {
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
                    expanded_items.push_back({col.name, col.name});
                }
            } else {
                std::string header;
                if (!item.alias.empty()) {
                    header = item.alias;
                } else if (item.expr->kind == ExprKind::COLUMN_REF) {
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
                expanded_items.push_back({header, item.expr->toSql()});
            }
        }

        std::vector<size_t> needed_cols;
        if (stmt.where_clause) {
            collectAstColumnIndices(stmt.where_clause.get(), stmt.from_table, schema, needed_cols);
            plan.has_filter = true;
            plan.where_sql = stmt.where_clause->toSql();
        }
        for (const auto& item : stmt.items) {
            if (item.expr->kind == ExprKind::STAR) {
                for (size_t c = 0; c < schema.size(); ++c) {
                    needed_cols.push_back(c);
                }
            } else {
                collectAstColumnIndices(item.expr.get(), stmt.from_table, schema, needed_cols);
            }
        }

        std::sort(needed_cols.begin(), needed_cols.end());
        needed_cols.erase(std::unique(needed_cols.begin(), needed_cols.end()), needed_cols.end());
        plan.needed_columns = needed_cols;
        for (size_t c : needed_cols) {
            plan.needed_column_names.push_back(schema[c].name);
        }

        std::unique_ptr<BoundExpr> bound_where;
        if (stmt.where_clause) {
            bound_where = bindExpr(stmt.where_clause.get(), stmt.from_table, schema, plan.literal_arena);
        }

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

    // -------------------------------------------------------------------------
    // Phase 3 path: Aggregate query
    // -------------------------------------------------------------------------
    plan.has_aggregate = true;

    // Check SELECT * in aggregate query
    for (const auto& item : stmt.items) {
        if (item.expr->kind == ExprKind::STAR) {
            std::cerr << "Error: SELECT * with GROUP BY or aggregation\n";
            std::exit(2);
        }
    }

    // Bind GROUP BY expressions
    std::vector<std::unique_ptr<BoundExpr>> bound_group_by;
    for (const auto& gb : stmt.group_by) {
        bound_group_by.push_back(bindExpr(gb.get(), stmt.from_table, schema, plan.literal_arena));
        plan.group_by_sqls.push_back(gb->toSql());
    }

    std::vector<AggSpec> agg_specs;

    // Recursive rewriter for expressions in SELECT items and HAVING
    std::function<std::unique_ptr<BoundExpr>(const Expr*)> rewrite = [&](const Expr* ast_expr) -> std::unique_ptr<BoundExpr> {
        if (!ast_expr) return nullptr;

        // Check if ast_expr matches any GROUP BY expression structurally
        if (!hasAggregateCall(ast_expr)) {
            auto candidate = bindExpr(ast_expr, stmt.from_table, schema, plan.literal_arena);
            for (size_t g = 0; g < bound_group_by.size(); ++g) {
                if (boundExprsEqual(candidate.get(), bound_group_by[g].get())) {
                    auto slot = std::make_unique<BoundExpr>();
                    slot->kind = BoundExprKind::SLOT_REF;
                    slot->slot_idx = g;
                    slot->affinity = candidate->affinity;
                    return slot;
                }
            }
        }

        // Aggregate function call
        if (ast_expr->kind == ExprKind::FUNCTION_CALL) {
            std::string name = toLower(ast_expr->func_name);
            if (!isAggregateFunction(name)) {
                std::cerr << "not implemented yet: scalar function: " << ast_expr->func_name << "\n";
                std::exit(3);
            }
            if (ast_expr->func_distinct) {
                std::cerr << "not implemented yet: COUNT(DISTINCT)\n";
                std::exit(3);
            }

            AggFunc func;
            if (name == "count") {
                func = ast_expr->func_star ? AggFunc::COUNT_STAR : AggFunc::COUNT;
            } else if (name == "sum") {
                func = AggFunc::SUM;
            } else if (name == "avg") {
                func = AggFunc::AVG;
            } else if (name == "min") {
                func = AggFunc::MIN;
            } else if (name == "max") {
                func = AggFunc::MAX;
            } else {
                std::cerr << "not implemented yet: aggregate function: " << ast_expr->func_name << "\n";
                std::exit(3);
            }

            if (ast_expr->func_star && func != AggFunc::COUNT_STAR) {
                std::cerr << "Error: misuse of aggregate function " << ast_expr->func_name << "()\n";
                std::exit(2);
            }

            std::unique_ptr<BoundExpr> bound_arg = nullptr;
            if (func != AggFunc::COUNT_STAR) {
                if (ast_expr->func_args.empty()) {
                    std::cerr << "Error: aggregate function " << ast_expr->func_name << " requires an argument\n";
                    std::exit(2);
                }
                if (ast_expr->func_args.size() > 1) {
                    std::cerr << "not implemented yet: scalar function: " << ast_expr->func_name << "\n";
                    std::exit(3);
                }
                const auto& arg_expr = ast_expr->func_args[0];
                if (hasAggregateCall(arg_expr.get())) {
                    std::cerr << "Error: misuse of aggregate function (nested aggregate)\n";
                    std::exit(2);
                }
                bound_arg = bindExpr(arg_expr.get(), stmt.from_table, schema, plan.literal_arena);
            }

            // Deduplicate AggSpec by structural equality
            int found_idx = -1;
            for (size_t i = 0; i < agg_specs.size(); ++i) {
                if (agg_specs[i].func == func) {
                    if (func == AggFunc::COUNT_STAR) {
                        found_idx = static_cast<int>(i);
                        break;
                    } else if (boundExprsEqual(agg_specs[i].arg.get(), bound_arg.get())) {
                        found_idx = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (found_idx < 0) {
                found_idx = static_cast<int>(agg_specs.size());
                AggSpec spec;
                spec.func = func;
                spec.arg = std::move(bound_arg);
                spec.sql = ast_expr->toSql();
                plan.aggregate_sqls.push_back(spec.sql);
                agg_specs.push_back(std::move(spec));
            }

            auto slot = std::make_unique<BoundExpr>();
            slot->kind = BoundExprKind::SLOT_REF;
            slot->slot_idx = bound_group_by.size() + found_idx;
            if (func == AggFunc::COUNT_STAR || func == AggFunc::COUNT) {
                slot->affinity = Affinity::NUMERIC;
            } else if (func == AggFunc::AVG) {
                slot->affinity = Affinity::NUMERIC;
            } else if (func == AggFunc::SUM) {
                slot->affinity = Affinity::NUMERIC;
            } else if (func == AggFunc::MIN || func == AggFunc::MAX) {
                if (agg_specs[found_idx].arg) {
                    slot->affinity = agg_specs[found_idx].arg->affinity;
                }
            }
            return slot;
        }

        // Bare column reference that did not match any GROUP BY expression
        if (ast_expr->kind == ExprKind::COLUMN_REF) {
            std::cerr << "Error: column '" << ast_expr->col_name
                      << "' must appear in the GROUP BY clause or be used in an aggregate function\n";
            std::exit(2);
        }

        // Literals
        if (ast_expr->kind == ExprKind::LITERAL_INT ||
            ast_expr->kind == ExprKind::LITERAL_FLOAT ||
            ast_expr->kind == ExprKind::LITERAL_STRING ||
            ast_expr->kind == ExprKind::LITERAL_NULL) {
            return bindExpr(ast_expr, stmt.from_table, schema, plan.literal_arena);
        }

        // Unary Op
        if (ast_expr->kind == ExprKind::UNARY_OP) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::UNARY_OP;
            b->op = toLower(ast_expr->op);
            b->left = rewrite(ast_expr->child.get());
            return b;
        }

        // Binary Op
        if (ast_expr->kind == ExprKind::BINARY_OP) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::BINARY_OP;
            b->op = toLower(ast_expr->op);
            b->left = rewrite(ast_expr->left.get());
            b->right = rewrite(ast_expr->right.get());
            return b;
        }

        // IS NULL
        if (ast_expr->kind == ExprKind::IS_NULL) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::IS_NULL;
            b->is_not = ast_expr->is_not;
            b->left = rewrite(ast_expr->child.get());
            return b;
        }

        // BETWEEN
        if (ast_expr->kind == ExprKind::BETWEEN) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::BETWEEN;
            b->is_not = ast_expr->is_not;
            b->left = rewrite(ast_expr->between_expr.get());
            b->between_low = rewrite(ast_expr->between_low.get());
            b->between_high = rewrite(ast_expr->between_high.get());
            return b;
        }

        // IN LIST
        if (ast_expr->kind == ExprKind::IN_LIST) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::IN_LIST;
            b->is_not = ast_expr->is_not;
            b->left = rewrite(ast_expr->in_expr.get());
            for (const auto& item : ast_expr->in_list) {
                b->in_list.push_back(rewrite(item.get()));
            }
            return b;
        }

        // LIKE
        if (ast_expr->kind == ExprKind::LIKE) {
            auto b = std::make_unique<BoundExpr>();
            b->kind = BoundExprKind::LIKE;
            b->is_not = ast_expr->is_not;
            b->left = rewrite(ast_expr->like_expr.get());
            b->like_pattern = rewrite(ast_expr->like_pattern.get());
            return b;
        }

        std::cerr << "Error: unhandled expression kind in aggregate query\n";
        std::exit(2);
    };

    // Rewrite SELECT items and determine headers
    std::vector<std::unique_ptr<BoundExpr>> bound_selects;
    for (const auto& item : stmt.items) {
        std::string header;
        if (!item.alias.empty()) {
            header = item.alias;
        } else if (item.expr->kind == ExprKind::COLUMN_REF) {
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
        plan.output_headers.push_back(header);
        plan.select_sqls.push_back(item.expr->toSql());
        bound_selects.push_back(rewrite(item.expr.get()));
    }

    // Rewrite HAVING if present
    std::unique_ptr<BoundExpr> bound_having = nullptr;
    if (stmt.having) {
        plan.has_having = true;
        plan.having_sql = stmt.having->toSql();
        bound_having = rewrite(stmt.having.get());
    }

    // Collect needed columns from WHERE, GROUP BY, and AggSpec arguments
    std::vector<size_t> needed_cols;
    if (stmt.where_clause) {
        collectAstColumnIndices(stmt.where_clause.get(), stmt.from_table, schema, needed_cols);
        plan.has_filter = true;
        plan.where_sql = stmt.where_clause->toSql();
    }
    for (const auto& gb : stmt.group_by) {
        collectAstColumnIndices(gb.get(), stmt.from_table, schema, needed_cols);
    }
    for (const auto& spec : agg_specs) {
        if (spec.arg) {
            collectBoundColumnIndices(spec.arg.get(), needed_cols);
        }
    }

    std::sort(needed_cols.begin(), needed_cols.end());
    needed_cols.erase(std::unique(needed_cols.begin(), needed_cols.end()), needed_cols.end());
    plan.needed_columns = needed_cols;
    for (size_t c : needed_cols) {
        plan.needed_column_names.push_back(schema[c].name);
    }

    // Bind WHERE condition against base schema
    std::unique_ptr<BoundExpr> bound_where;
    if (stmt.where_clause) {
        bound_where = bindExpr(stmt.where_clause.get(), stmt.from_table, schema, plan.literal_arena);
    }

    // Assemble operator pipeline:
    // Scan -> [Filter (WHERE)] -> Aggregate -> [Filter (HAVING)] -> Project -> [Limit]
    std::unique_ptr<Operator> op = std::make_unique<ScanOperator>(
        plan.csv_path,
        plan.needed_columns,
        col_types,
        all_col_names
    );

    if (bound_where) {
        op = std::make_unique<FilterOperator>(std::move(op), std::move(bound_where));
    }

    op = std::make_unique<AggregateOperator>(
        std::move(op),
        std::move(bound_group_by),
        std::move(agg_specs),
        max_groups
    );

    if (bound_having) {
        op = std::make_unique<FilterOperator>(std::move(op), std::move(bound_having));
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

    int indent = 2;
    if (has_filter) {
        std::fprintf(out, "%*sFilter (where: %s)\n", indent, "", where_sql.c_str());
        indent += 2;
    }

    if (has_aggregate) {
        std::fprintf(out, "%*sAggregate (group_by: [", indent, "");
        for (size_t i = 0; i < group_by_sqls.size(); ++i) {
            if (i > 0) std::fprintf(out, ", ");
            std::fprintf(out, "%s", group_by_sqls[i].c_str());
        }
        std::fprintf(out, "], aggregates: [");
        for (size_t i = 0; i < aggregate_sqls.size(); ++i) {
            if (i > 0) std::fprintf(out, ", ");
            std::fprintf(out, "%s", aggregate_sqls[i].c_str());
        }
        std::fprintf(out, "])\n");
        indent += 2;

        if (has_having) {
            std::fprintf(out, "%*sFilter (having: %s)\n", indent, "", having_sql.c_str());
            indent += 2;
        }
    }

    std::fprintf(out, "%*sProject (columns: [", indent, "");
    for (size_t i = 0; i < select_sqls.size(); ++i) {
        if (i > 0) std::fprintf(out, ", ");
        std::fprintf(out, "%s", select_sqls[i].c_str());
        if (output_headers[i] != select_sqls[i]) {
            std::fprintf(out, " AS %s", output_headers[i].c_str());
        }
    }
    std::fprintf(out, "])\n");
    indent += 2;

    if (has_limit) {
        std::fprintf(out, "%*sLimit (limit: %lld, offset: %lld)\n", indent, "", limit, offset);
    }
}

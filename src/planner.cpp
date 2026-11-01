#include "planner.h"
#include "ops_agg.h"
#include "ops_sort.h"
#include "ops_distinct.h"
#include "ops_join.h"

#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cassert>
#include <limits>
#include <set>
#include <map>
#include <sys/stat.h>

static inline std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) {
        c = asciiToLower(c);
    }
    return lower;
}

static size_t getFileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<size_t>(st.st_size);
    }
    return 0;
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

static void splitConjunction(const Expr* expr, std::vector<const Expr*>& conjuncts) {
    if (!expr) return;
    if (expr->kind == ExprKind::BINARY_OP && toLower(expr->op) == "and") {
        splitConjunction(expr->left.get(), conjuncts);
        splitConjunction(expr->right.get(), conjuncts);
    } else {
        conjuncts.push_back(expr);
    }
}

static std::unique_ptr<BoundExpr> combineBoundConjunction(std::vector<std::unique_ptr<BoundExpr>> conjuncts) {
    if (conjuncts.empty()) return nullptr;
    auto root = std::move(conjuncts[0]);
    for (size_t i = 1; i < conjuncts.size(); ++i) {
        auto node = std::make_unique<BoundExpr>();
        node->kind = BoundExprKind::BINARY_OP;
        node->op = "and";
        node->left = std::move(root);
        node->right = std::move(conjuncts[i]);
        node->affinity = Affinity::NONE;
        root = std::move(node);
    }
    return root;
}

static void collectReferencedTables(
    const Expr* expr,
    const MultiTableScope& scope,
    std::set<size_t>& out_tables
) {
    if (!expr) return;

    if (expr->kind == ExprKind::COLUMN_REF) {
        std::string target_lower = toLower(expr->col_name);
        if (!expr->table_name.empty()) {
            std::string prefix_lower = toLower(expr->table_name);
            const TableScope* matched_table = nullptr;
            size_t matched_t = 0;
            for (size_t t = 0; t < scope.tables.size(); ++t) {
                const auto& ts = scope.tables[t];
                std::string alias_lower = toLower(ts.alias);
                std::string name_lower = toLower(ts.name);

                if (!alias_lower.empty()) {
                    if (prefix_lower == alias_lower) {
                        matched_table = &ts;
                        matched_t = t;
                        break;
                    } else if (prefix_lower == name_lower) {
                        std::cerr << "Error: table alias '" << ts.alias << "' hides table name '" << ts.name << "'\n";
                        std::exit(2);
                    }
                } else {
                    if (prefix_lower == name_lower) {
                        matched_table = &ts;
                        matched_t = t;
                        break;
                    }
                }
            }

            if (!matched_table) {
                std::cerr << "Error: unknown table qualifier '" << expr->table_name << "'\n";
                std::exit(2);
            }

            int found_c = -1;
            for (size_t c = 0; c < matched_table->schema.size(); ++c) {
                if (toLower(matched_table->schema[c].name) == target_lower) {
                    found_c = static_cast<int>(c);
                    break;
                }
            }
            if (found_c < 0) {
                std::cerr << "Error: unknown column '" << expr->col_name << "'\n";
                std::exit(2);
            }
            out_tables.insert(matched_t);
            return;
        } else {
            // Unqualified column reference
            int match_count = 0;
            size_t found_t = 0;
            for (size_t t = 0; t < scope.tables.size(); ++t) {
                const auto& ts = scope.tables[t];
                for (size_t c = 0; c < ts.schema.size(); ++c) {
                    if (toLower(ts.schema[c].name) == target_lower) {
                        match_count++;
                        found_t = t;
                        break;
                    }
                }
            }
            if (match_count == 0) {
                std::cerr << "Error: unknown column '" << expr->col_name << "'\n";
                std::exit(2);
            } else if (match_count > 1) {
                std::cerr << "Error: ambiguous column name: " << expr->col_name << "\n";
                std::exit(2);
            }
            out_tables.insert(found_t);
            return;
        }
    }

    if (expr->kind == ExprKind::FUNCTION_CALL) {
        for (const auto& arg : expr->func_args) {
            collectReferencedTables(arg.get(), scope, out_tables);
        }
        return;
    }

    collectReferencedTables(expr->child.get(), scope, out_tables);
    collectReferencedTables(expr->left.get(), scope, out_tables);
    collectReferencedTables(expr->right.get(), scope, out_tables);
    collectReferencedTables(expr->between_expr.get(), scope, out_tables);
    collectReferencedTables(expr->between_low.get(), scope, out_tables);
    collectReferencedTables(expr->between_high.get(), scope, out_tables);
    collectReferencedTables(expr->in_expr.get(), scope, out_tables);
    for (const auto& item : expr->in_list) {
        collectReferencedTables(item.get(), scope, out_tables);
    }
    collectReferencedTables(expr->like_expr.get(), scope, out_tables);
    collectReferencedTables(expr->like_pattern.get(), scope, out_tables);
}

static void collectTableNeededColumns(
    const Expr* expr,
    const MultiTableScope& scope,
    std::vector<std::vector<size_t>>& table_needed_cols
) {
    if (!expr) return;

    if (expr->kind == ExprKind::COLUMN_REF) {
        std::string target_lower = toLower(expr->col_name);
        if (!expr->table_name.empty()) {
            std::string prefix_lower = toLower(expr->table_name);
            const TableScope* matched_table = nullptr;
            size_t matched_t = 0;
            for (size_t t = 0; t < scope.tables.size(); ++t) {
                const auto& ts = scope.tables[t];
                std::string alias_lower = toLower(ts.alias);
                std::string name_lower = toLower(ts.name);

                if (!alias_lower.empty()) {
                    if (prefix_lower == alias_lower) {
                        matched_table = &ts;
                        matched_t = t;
                        break;
                    } else if (prefix_lower == name_lower) {
                        std::cerr << "Error: table alias '" << ts.alias << "' hides table name '" << ts.name << "'\n";
                        std::exit(2);
                    }
                } else {
                    if (prefix_lower == name_lower) {
                        matched_table = &ts;
                        matched_t = t;
                        break;
                    }
                }
            }

            if (!matched_table) {
                std::cerr << "Error: unknown table qualifier '" << expr->table_name << "'\n";
                std::exit(2);
            }

            int found_c = -1;
            for (size_t c = 0; c < matched_table->schema.size(); ++c) {
                if (toLower(matched_table->schema[c].name) == target_lower) {
                    found_c = static_cast<int>(c);
                    break;
                }
            }
            if (found_c < 0) {
                std::cerr << "Error: unknown column '" << expr->col_name << "'\n";
                std::exit(2);
            }
            table_needed_cols[matched_t].push_back(static_cast<size_t>(found_c));
            return;
        } else {
            // Unqualified column reference
            int match_count = 0;
            size_t found_t = 0;
            size_t found_c = 0;
            for (size_t t = 0; t < scope.tables.size(); ++t) {
                const auto& ts = scope.tables[t];
                for (size_t c = 0; c < ts.schema.size(); ++c) {
                    if (toLower(ts.schema[c].name) == target_lower) {
                        match_count++;
                        found_t = t;
                        found_c = c;
                        break;
                    }
                }
            }
            if (match_count == 0) {
                std::cerr << "Error: unknown column '" << expr->col_name << "'\n";
                std::exit(2);
            } else if (match_count > 1) {
                std::cerr << "Error: ambiguous column name: " << expr->col_name << "\n";
                std::exit(2);
            }
            table_needed_cols[found_t].push_back(found_c);
            return;
        }
    }

    if (expr->kind == ExprKind::FUNCTION_CALL) {
        for (const auto& arg : expr->func_args) {
            collectTableNeededColumns(arg.get(), scope, table_needed_cols);
        }
        return;
    }

    collectTableNeededColumns(expr->child.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->left.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->right.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->between_expr.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->between_low.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->between_high.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->in_expr.get(), scope, table_needed_cols);
    for (const auto& item : expr->in_list) {
        collectTableNeededColumns(item.get(), scope, table_needed_cols);
    }
    collectTableNeededColumns(expr->like_expr.get(), scope, table_needed_cols);
    collectTableNeededColumns(expr->like_pattern.get(), scope, table_needed_cols);
}

static void collectBoundTableNeededColumns(
    const BoundExpr* expr,
    const MultiTableScope& scope,
    std::vector<std::vector<size_t>>& table_needed_cols
) {
    if (!expr) return;
    if (expr->kind == BoundExprKind::COLUMN_REF) {
        size_t idx = expr->col_idx;
        for (size_t t = 0; t < scope.tables.size(); ++t) {
            const auto& ts = scope.tables[t];
            if (idx >= ts.tuple_offset && idx < ts.tuple_offset + ts.tuple_width) {
                table_needed_cols[t].push_back(idx - ts.tuple_offset);
                break;
            }
        }
        return;
    }
    collectBoundTableNeededColumns(expr->left.get(), scope, table_needed_cols);
    collectBoundTableNeededColumns(expr->right.get(), scope, table_needed_cols);
    collectBoundTableNeededColumns(expr->between_low.get(), scope, table_needed_cols);
    collectBoundTableNeededColumns(expr->between_high.get(), scope, table_needed_cols);
    for (const auto& item : expr->in_list) {
        collectBoundTableNeededColumns(item.get(), scope, table_needed_cols);
    }
    collectBoundTableNeededColumns(expr->like_pattern.get(), scope, table_needed_cols);
}

static std::unique_ptr<BoundExpr> rewritePostAggregateExpr(
    const Expr* ast_expr,
    const MultiTableScope& scope,
    const std::vector<std::unique_ptr<BoundExpr>>& bound_group_by,
    std::vector<AggSpec>& agg_specs,
    Plan& plan
) {
    if (!ast_expr) return nullptr;

    // Check if ast_expr matches any GROUP BY expression structurally
    if (!hasAggregateCall(ast_expr)) {
        auto candidate = bindExpr(ast_expr, scope, plan.literal_arena);
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
            bound_arg = bindExpr(arg_expr.get(), scope, plan.literal_arena);
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
        return bindExpr(ast_expr, scope, plan.literal_arena);
    }

    // Unary Op
    if (ast_expr->kind == ExprKind::UNARY_OP) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::UNARY_OP;
        b->op = toLower(ast_expr->op);
        b->left = rewritePostAggregateExpr(ast_expr->child.get(), scope, bound_group_by, agg_specs, plan);
        return b;
    }

    // Binary Op
    if (ast_expr->kind == ExprKind::BINARY_OP) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::BINARY_OP;
        b->op = toLower(ast_expr->op);
        b->left = rewritePostAggregateExpr(ast_expr->left.get(), scope, bound_group_by, agg_specs, plan);
        b->right = rewritePostAggregateExpr(ast_expr->right.get(), scope, bound_group_by, agg_specs, plan);
        return b;
    }

    // IS NULL
    if (ast_expr->kind == ExprKind::IS_NULL) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::IS_NULL;
        b->is_not = ast_expr->is_not;
        b->left = rewritePostAggregateExpr(ast_expr->child.get(), scope, bound_group_by, agg_specs, plan);
        return b;
    }

    // BETWEEN
    if (ast_expr->kind == ExprKind::BETWEEN) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::BETWEEN;
        b->is_not = ast_expr->is_not;
        b->left = rewritePostAggregateExpr(ast_expr->between_expr.get(), scope, bound_group_by, agg_specs, plan);
        b->between_low = rewritePostAggregateExpr(ast_expr->between_low.get(), scope, bound_group_by, agg_specs, plan);
        b->between_high = rewritePostAggregateExpr(ast_expr->between_high.get(), scope, bound_group_by, agg_specs, plan);
        return b;
    }

    // IN LIST
    if (ast_expr->kind == ExprKind::IN_LIST) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::IN_LIST;
        b->is_not = ast_expr->is_not;
        b->left = rewritePostAggregateExpr(ast_expr->in_expr.get(), scope, bound_group_by, agg_specs, plan);
        for (const auto& item : ast_expr->in_list) {
            b->in_list.push_back(rewritePostAggregateExpr(item.get(), scope, bound_group_by, agg_specs, plan));
        }
        return b;
    }

    // LIKE
    if (ast_expr->kind == ExprKind::LIKE) {
        auto b = std::make_unique<BoundExpr>();
        b->kind = BoundExprKind::LIKE;
        b->is_not = ast_expr->is_not;
        b->left = rewritePostAggregateExpr(ast_expr->like_expr.get(), scope, bound_group_by, agg_specs, plan);
        b->like_pattern = rewritePostAggregateExpr(ast_expr->like_pattern.get(), scope, bound_group_by, agg_specs, plan);
        return b;
    }

    std::cerr << "Error: unhandled expression kind in aggregate query\n";
    std::exit(2);
}

bool isTablePushdownLegal(size_t table_idx, const std::vector<JoinClause>& joins) {
    for (size_t j = 0; j < joins.size(); ++j) {
        JoinType jt = joins[j].type;
        if (jt == JoinType::LEFT || jt == JoinType::FULL) {
            if (table_idx == j + 1) return false;
        }
        if (jt == JoinType::RIGHT || jt == JoinType::FULL) {
            if (table_idx <= j) return false;
        }
    }
    return true;
}

Plan buildPlan(const SelectStmt& stmt, const std::vector<std::string>& table_args, const PlanOptions& options) {
    Plan plan;

    if (table_args.empty()) {
        std::cerr << "Error: missing --table argument for SQL query\n";
        std::exit(2);
    }

    // Parse all table_args into name -> csv_path
    struct TableArgInfo {
        std::string name;
        std::string csv_path;
    };
    std::map<std::string, TableArgInfo> table_map; // lowercase name -> info
    for (const auto& arg : table_args) {
        std::string name = "t";
        std::string path = arg;
        size_t eq_pos = arg.find('=');
        if (eq_pos != std::string::npos) {
            name = arg.substr(0, eq_pos);
            path = arg.substr(eq_pos + 1);
        }
        std::string lower_name = toLower(name);
        if (table_map.find(lower_name) != table_map.end()) {
            std::cerr << "Error: duplicate table name '" << name << "'\n";
            std::exit(2);
        }
        table_map[lower_name] = {name, path};
    }

    // Check unsupported join features early (exit 3)
    for (const auto& j : stmt.joins) {
        if (j.is_natural) {
            std::cerr << "not implemented yet: NATURAL JOIN\n";
            std::exit(3);
        }
        if (j.is_using) {
            std::cerr << "not implemented yet: USING\n";
            std::exit(3);
        }
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

    // Build MultiTableScope from query's FROM table and JOIN tables
    MultiTableScope scope;
    std::vector<TableRef> query_tables;
    query_tables.push_back(stmt.from_table);
    for (const auto& j : stmt.joins) {
        query_tables.push_back(j.table);
    }

    std::set<std::string> seen_names;
    for (size_t t = 0; t < query_tables.size(); ++t) {
        const auto& tbl = query_tables[t];
        std::string lower_lookup = toLower(tbl.name);
        auto it = table_map.find(lower_lookup);
        if (it == table_map.end()) {
            std::cerr << "Error: unknown table '" << tbl.name << "'\n";
            std::exit(2);
        }

        // Check duplicate effective name or alias
        std::string effective_name = !tbl.alias.empty() ? tbl.alias : tbl.name;
        std::string effective_lower = toLower(effective_name);
        if (seen_names.count(effective_lower)) {
            std::cerr << "Error: duplicate table name or alias '" << effective_name << "'\n";
            std::exit(2);
        }
        seen_names.insert(effective_lower);
        if (!tbl.alias.empty()) {
            std::string alias_lower = toLower(tbl.alias);
            if (seen_names.count(alias_lower) && alias_lower != effective_lower) {
                std::cerr << "Error: duplicate table alias '" << tbl.alias << "'\n";
                std::exit(2);
            }
            seen_names.insert(alias_lower);
        }

        CsvScanner scanner;
        if (!scanner.open(it->second.csv_path.c_str())) {
            std::cerr << "Error: cannot open or read file '" << it->second.csv_path << "'\n";
            std::exit(2);
        }
        TableScope ts;
        ts.name = it->second.name;
        ts.alias = tbl.alias;
        ts.schema = scanner.columns();
        ts.tuple_offset = scope.total_width;
        ts.tuple_width = ts.schema.size();
        ts.csv_path = it->second.csv_path;
        scope.total_width += ts.tuple_width;
        scope.tables.push_back(std::move(ts));
        scanner.close();
    }

    plan.table_name = scope.tables[0].name;
    plan.csv_path = scope.tables[0].csv_path;

    // Validate ON clause scopes (tables 0..i only) and no aggregates in ON
    for (size_t i = 1; i < scope.tables.size(); ++i) {
        const auto& j = stmt.joins[i - 1];
        if (hasAggregateCall(j.on_expr.get())) {
            std::cerr << "Error: misuse of aggregate function in ON clause\n";
            std::exit(2);
        }
        MultiTableScope scope_up_to_i;
        for (size_t k = 0; k <= i; ++k) {
            TableScope copy = scope.tables[k];
            scope_up_to_i.tables.push_back(std::move(copy));
        }
        std::set<size_t> ref_tables;
        collectReferencedTables(j.on_expr.get(), scope_up_to_i, ref_tables);
    }

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
            std::cerr << "not implemented yet: HAVING clause on a non-aggregate query\n";
            std::exit(3);
        }
    }
    if (!is_aggregate_query) {
        for (const auto& ob : stmt.order_by) {
            if (hasAggregateCall(ob.expr.get())) {
                std::cerr << "Error: misuse of aggregate function in ORDER BY\n";
                std::exit(2);
            }
        }
    }

    // Split WHERE into conjuncts and classify pushability
    std::vector<const Expr*> where_conjuncts;
    if (stmt.where_clause) {
        splitConjunction(stmt.where_clause.get(), where_conjuncts);
    }

    std::vector<std::vector<const Expr*>> pushed_where(scope.tables.size());
    std::vector<const Expr*> unpushed_where;

    for (const auto* c : where_conjuncts) {
        std::set<size_t> ref_tables;
        collectReferencedTables(c, scope, ref_tables);
        if (ref_tables.size() == 1) {
            size_t t = *ref_tables.begin();
            if (!options.no_pushdown && isTablePushdownLegal(t, stmt.joins)) {
                pushed_where[t].push_back(c);
            } else {
                unpushed_where.push_back(c);
            }
        } else {
            unpushed_where.push_back(c);
        }
    }

    // Query processing: non-aggregate vs aggregate
    size_t M = scope.tables.size();
    std::vector<std::vector<size_t>> table_needed_cols(M);

    // Track needed columns from pushed and unpushed WHERE
    for (size_t t = 0; t < M; ++t) {
        for (const auto* c : pushed_where[t]) {
            collectTableNeededColumns(c, scope, table_needed_cols);
        }
    }
    for (const auto* c : unpushed_where) {
        collectTableNeededColumns(c, scope, table_needed_cols);
    }

    if (!is_aggregate_query) {
        // ---------------------------------------------------------------------
        // Non-aggregate query path
        // ---------------------------------------------------------------------
        if (stmt.having) {
            std::cerr << "not implemented yet: HAVING clause on a non-aggregate query\n";
            std::exit(3);
        }

        struct ExpandedItem {
            std::string header;
            std::string sql;
            std::unique_ptr<BoundExpr> expr;
        };
        std::vector<ExpandedItem> expanded_items;
        std::vector<size_t> item_slot_map;

        for (const auto& item : stmt.items) {
            item_slot_map.push_back(expanded_items.size());
            if (item.expr->kind == ExprKind::STAR) {
                if (!item.expr->table_name.empty()) {
                    // Qualified STAR (e.g. t.* or alias.*)
                    std::string prefix_lower = toLower(item.expr->table_name);
                    const TableScope* matched_t = nullptr;
                    for (const auto& ts : scope.tables) {
                        std::string alias_lower = toLower(ts.alias);
                        std::string name_lower = toLower(ts.name);
                        if (!alias_lower.empty()) {
                            if (prefix_lower == alias_lower) {
                                matched_t = &ts;
                                break;
                            } else if (prefix_lower == name_lower) {
                                std::cerr << "Error: table alias '" << ts.alias << "' hides table name '" << ts.name << "'\n";
                                std::exit(2);
                            }
                        } else {
                            if (prefix_lower == name_lower) {
                                matched_t = &ts;
                                break;
                            }
                        }
                    }
                    if (!matched_t) {
                        std::cerr << "Error: unknown table qualifier '" << item.expr->table_name << "'\n";
                        std::exit(2);
                    }
                    for (size_t c = 0; c < matched_t->schema.size(); ++c) {
                        auto b_col = std::make_unique<BoundExpr>();
                        b_col->kind = BoundExprKind::COLUMN_REF;
                        b_col->col_idx = matched_t->tuple_offset + c;
                        b_col->col_name = matched_t->schema[c].name;
                        b_col->affinity = (matched_t->schema[c].type == TYPE_STRING) ? Affinity::TEXT : Affinity::NUMERIC;
                        expanded_items.push_back({matched_t->schema[c].name, matched_t->schema[c].name, std::move(b_col)});
                    }
                } else {
                    // Unqualified STAR: expands all tables in scope in join order
                    for (const auto& ts : scope.tables) {
                        for (size_t c = 0; c < ts.schema.size(); ++c) {
                            auto b_col = std::make_unique<BoundExpr>();
                            b_col->kind = BoundExprKind::COLUMN_REF;
                            b_col->col_idx = ts.tuple_offset + c;
                            b_col->col_name = ts.schema[c].name;
                            b_col->affinity = (ts.schema[c].type == TYPE_STRING) ? Affinity::TEXT : Affinity::NUMERIC;
                            expanded_items.push_back({ts.schema[c].name, ts.schema[c].name, std::move(b_col)});
                        }
                    }
                }
            } else {
                std::string header;
                if (!item.alias.empty()) {
                    header = item.alias;
                } else if (item.expr->kind == ExprKind::COLUMN_REF) {
                    header = item.expr->col_name;
                } else {
                    header = item.expr->toSql();
                }
                auto b_expr = bindExpr(item.expr.get(), scope, plan.literal_arena);
                if (item.expr->kind == ExprKind::COLUMN_REF && item.alias.empty()) {
                    header = b_expr->col_name;
                }
                expanded_items.push_back({header, item.expr->toSql(), std::move(b_expr)});
            }
        }

        std::vector<std::unique_ptr<BoundExpr>> bound_selects;
        for (auto& exp : expanded_items) {
            plan.output_headers.push_back(exp.header);
            plan.select_sqls.push_back(exp.sql);
            bound_selects.push_back(std::move(exp.expr));
        }

        size_t N = bound_selects.size();
        plan.visible_width = N;

        // Process ORDER BY terms for non-aggregate query
        std::vector<SortTerm> sort_terms;
        std::vector<std::unique_ptr<BoundExpr>> hidden_exprs;

        for (const auto& ob_item : stmt.order_by) {
            const Expr* expr = ob_item.expr.get();
            int found_slot = -1;

            // Step 1: Integer literal ordinal
            if (expr->kind == ExprKind::LITERAL_INT) {
                long long k = expr->int_val;
                if (k < 1 || k > static_cast<long long>(N)) {
                    std::cerr << "Error: ORDER BY term out of range\n";
                    std::exit(2);
                }
                found_slot = static_cast<int>(k - 1);
            }

            // Step 2: Bare unqualified identifier matching select item alias
            if (found_slot < 0 && expr->kind == ExprKind::COLUMN_REF && expr->table_name.empty()) {
                std::string col_lower = toLower(expr->col_name);
                for (size_t i = 0; i < stmt.items.size(); ++i) {
                    if (!stmt.items[i].alias.empty() && toLower(stmt.items[i].alias) == col_lower) {
                        found_slot = static_cast<int>(item_slot_map[i]);
                        break;
                    }
                }
            }

            // Step 3: Expression binding
            if (found_slot < 0) {
                auto bound_ob = bindExpr(expr, scope, plan.literal_arena);
                for (size_t s = 0; s < N; ++s) {
                    if (boundExprsEqual(bound_ob.get(), bound_selects[s].get())) {
                        found_slot = static_cast<int>(s);
                        break;
                    }
                }
                if (found_slot < 0) {
                    if (stmt.distinct) {
                        std::cerr << "Error: for SELECT DISTINCT, ORDER BY terms must appear in the select list\n";
                        std::exit(2);
                    }
                    for (size_t h = 0; h < hidden_exprs.size(); ++h) {
                        if (boundExprsEqual(bound_ob.get(), hidden_exprs[h].get())) {
                            found_slot = static_cast<int>(N + h);
                            break;
                        }
                    }
                    if (found_slot < 0) {
                        found_slot = static_cast<int>(N + hidden_exprs.size());
                        hidden_exprs.push_back(std::move(bound_ob));
                    }
                }
            }

            sort_terms.push_back({static_cast<size_t>(found_slot), ob_item.descending});
            plan.sort_terms.push_back({expr->toSql(), ob_item.descending});
        }

        if (stmt.distinct) {
            assert(hidden_exprs.empty());
        }

        plan.hidden_columns_count = hidden_exprs.size();
        for (size_t h = 0; h < hidden_exprs.size(); ++h) {
            collectBoundTableNeededColumns(hidden_exprs[h].get(), scope, table_needed_cols);
            bound_selects.push_back(std::move(hidden_exprs[h]));
            plan.output_headers.push_back("hidden_" + std::to_string(h));
        }

        // Track needed columns from select items
        for (const auto& item : stmt.items) {
            if (item.expr->kind == ExprKind::STAR) {
                if (!item.expr->table_name.empty()) {
                    std::string prefix_lower = toLower(item.expr->table_name);
                    for (size_t t = 0; t < scope.tables.size(); ++t) {
                        const auto& ts = scope.tables[t];
                        std::string alias_lower = toLower(ts.alias);
                        std::string name_lower = toLower(ts.name);
                        if ((!alias_lower.empty() && prefix_lower == alias_lower) ||
                            (alias_lower.empty() && prefix_lower == name_lower)) {
                            for (size_t c = 0; c < ts.schema.size(); ++c) {
                                table_needed_cols[t].push_back(c);
                            }
                            break;
                        }
                    }
                } else {
                    for (size_t t = 0; t < scope.tables.size(); ++t) {
                        for (size_t c = 0; c < scope.tables[t].schema.size(); ++c) {
                            table_needed_cols[t].push_back(c);
                        }
                    }
                }
            } else {
                collectTableNeededColumns(item.expr.get(), scope, table_needed_cols);
            }
        }

        // Collect needed columns from all ON clauses
        for (const auto& j : stmt.joins) {
            collectTableNeededColumns(j.on_expr.get(), scope, table_needed_cols);
        }

        // Finalize needed columns per table
        for (size_t t = 0; t < M; ++t) {
            std::sort(table_needed_cols[t].begin(), table_needed_cols[t].end());
            table_needed_cols[t].erase(std::unique(table_needed_cols[t].begin(), table_needed_cols[t].end()), table_needed_cols[t].end());
        }
        plan.needed_columns = table_needed_cols[0];
        for (size_t c : table_needed_cols[0]) {
            plan.needed_column_names.push_back(scope.tables[0].schema[c].name);
        }

        // Construct base table scans
        std::vector<std::unique_ptr<Operator>> table_scans;
        for (size_t t = 0; t < M; ++t) {
            const auto& ts = scope.tables[t];
            std::vector<ColType> col_types;
            std::vector<std::string> all_col_names;
            for (const auto& col : ts.schema) {
                col_types.push_back(col.type);
                all_col_names.push_back(col.name);
            }

            std::unique_ptr<Operator> scan_op = std::make_unique<ScanOperator>(
                ts.csv_path,
                table_needed_cols[t],
                col_types,
                all_col_names
            );

            if (!pushed_where[t].empty()) {
                MultiTableScope local_scope;
                TableScope copy = ts;
                copy.tuple_offset = 0;
                local_scope.tables.push_back(std::move(copy));
                local_scope.total_width = ts.schema.size();

                std::vector<std::unique_ptr<BoundExpr>> pushed_bounds;
                for (const auto* c : pushed_where[t]) {
                    pushed_bounds.push_back(bindExpr(c, local_scope, plan.literal_arena));
                }
                auto bound_pushed = combineBoundConjunction(std::move(pushed_bounds));
                scan_op = std::make_unique<FilterOperator>(std::move(scan_op), std::move(bound_pushed));
            }
            table_scans.push_back(std::move(scan_op));
        }

        // Assemble join tree
        std::unique_ptr<Operator> current_op = std::move(table_scans[0]);
        size_t current_probe_width = scope.tables[0].tuple_width;

        for (size_t i = 1; i < M; ++i) {
            const auto& j = stmt.joins[i - 1];
            std::vector<const Expr*> on_conjuncts;
            splitConjunction(j.on_expr.get(), on_conjuncts);

            // Scopes for binding
            MultiTableScope scope_probe;
            for (size_t k = 0; k < i; ++k) {
                TableScope copy = scope.tables[k];
                scope_probe.tables.push_back(std::move(copy));
                scope_probe.total_width += copy.tuple_width;
            }

            MultiTableScope local_scope_i;
            TableScope copy_i = scope.tables[i];
            copy_i.tuple_offset = 0;
            local_scope_i.tables.push_back(std::move(copy_i));
            local_scope_i.total_width = copy_i.tuple_width;

            MultiTableScope scope_up_to_i;
            for (size_t k = 0; k <= i; ++k) {
                TableScope copy = scope.tables[k];
                scope_up_to_i.tables.push_back(std::move(copy));
                scope_up_to_i.total_width += copy.tuple_width;
            }

            std::vector<std::unique_ptr<BoundExpr>> probe_keys;
            std::vector<std::unique_ptr<BoundExpr>> build_keys;
            std::vector<Affinity> probe_affs;
            std::vector<Affinity> build_affs;
            std::vector<std::pair<std::string, std::string>> equi_key_sqls;
            std::vector<const Expr*> residual_conjuncts;

            for (const auto* c : on_conjuncts) {
                bool is_equi = false;
                if (c->kind == ExprKind::BINARY_OP && c->op == "=") {
                    std::set<size_t> left_tables;
                    std::set<size_t> right_tables;
                    collectReferencedTables(c->left.get(), scope_up_to_i, left_tables);
                    collectReferencedTables(c->right.get(), scope_up_to_i, right_tables);

                    if (!left_tables.empty() && !right_tables.empty()) {
                        bool left_is_probe = (*left_tables.rbegin() < i);
                        bool right_is_build = (right_tables.size() == 1 && *right_tables.begin() == i);

                        bool right_is_probe = (*right_tables.rbegin() < i);
                        bool left_is_build = (left_tables.size() == 1 && *left_tables.begin() == i);

                        if (left_is_probe && right_is_build) {
                            auto p_key = bindExpr(c->left.get(), scope_probe, plan.literal_arena);
                            auto b_key = bindExpr(c->right.get(), local_scope_i, plan.literal_arena);
                            probe_affs.push_back(p_key->affinity);
                            build_affs.push_back(b_key->affinity);
                            probe_keys.push_back(std::move(p_key));
                            build_keys.push_back(std::move(b_key));
                            equi_key_sqls.push_back({c->left->toSql(), c->right->toSql()});
                            is_equi = true;
                        } else if (right_is_probe && left_is_build) {
                            auto p_key = bindExpr(c->right.get(), scope_probe, plan.literal_arena);
                            auto b_key = bindExpr(c->left.get(), local_scope_i, plan.literal_arena);
                            probe_affs.push_back(p_key->affinity);
                            build_affs.push_back(b_key->affinity);
                            probe_keys.push_back(std::move(p_key));
                            build_keys.push_back(std::move(b_key));
                            equi_key_sqls.push_back({c->right->toSql(), c->left->toSql()});
                            is_equi = true;
                        }
                    }
                }
                if (!is_equi) {
                    residual_conjuncts.push_back(c);
                }
            }

            std::unique_ptr<BoundExpr> residual_pred = nullptr;
            std::string residual_sql;
            if (!residual_conjuncts.empty()) {
                std::vector<std::unique_ptr<BoundExpr>> bound_res;
                for (const auto* r : residual_conjuncts) {
                    bound_res.push_back(bindExpr(r, scope_up_to_i, plan.literal_arena));
                    if (!residual_sql.empty()) residual_sql += " AND ";
                    residual_sql += r->toSql();
                }
                residual_pred = combineBoundConjunction(std::move(bound_res));
            }

            bool is_nested_loop = probe_keys.empty();

            // Build swap heuristic: if !options.no_join_swap && i == 1 && (INNER || RIGHT) && fileSize(table 0) < fileSize(table 1)
            bool swapped = false;
            if (!options.no_join_swap && i == 1 && (j.type == JoinType::INNER || j.type == JoinType::RIGHT)) {
                size_t sz0 = getFileSize(scope.tables[0].csv_path);
                size_t sz1 = getFileSize(scope.tables[1].csv_path);
                if (sz0 < sz1) {
                    swapped = true;
                }
            }

            bool emit_unmatched_probe = false;
            bool emit_unmatched_build = false;
            if (j.type == JoinType::INNER) {
                emit_unmatched_probe = false;
                emit_unmatched_build = false;
            } else if (j.type == JoinType::LEFT) {
                emit_unmatched_probe = true;
                emit_unmatched_build = false;
            } else if (j.type == JoinType::RIGHT) {
                if (swapped) {
                    // Swapped RIGHT: probe is table 1 (R, preserved), build is table 0 (L)
                    emit_unmatched_probe = true;
                    emit_unmatched_build = false;
                } else {
                    // Default RIGHT: probe is L, build is R (preserved)
                    emit_unmatched_probe = false;
                    emit_unmatched_build = true;
                }
            } else if (j.type == JoinType::FULL) {
                emit_unmatched_probe = true;
                emit_unmatched_build = true;
                swapped = false; // never swapped
            }

            // Kept build slots: columns of build child needed above this join
            std::vector<size_t> kept_build_slots;
            size_t build_table_idx = swapped ? 0 : i;

            // Columns needed in selects, unpushed WHERE, subsequent joins, order by, and residual
            std::set<size_t> kept_set;
            for (size_t c : table_needed_cols[build_table_idx]) {
                kept_set.insert(c);
            }
            for (size_t slot : kept_set) {
                kept_build_slots.push_back(slot);
            }
            std::sort(kept_build_slots.begin(), kept_build_slots.end());

            Plan::JoinPlanInfo join_info;
            switch (j.type) {
                case JoinType::INNER: join_info.join_type = "INNER"; break;
                case JoinType::LEFT:  join_info.join_type = "LEFT";  break;
                case JoinType::RIGHT: join_info.join_type = "RIGHT"; break;
                case JoinType::FULL:  join_info.join_type = "FULL";  break;
            }
            join_info.swapped = swapped;
            join_info.is_nested_loop = is_nested_loop;
            join_info.equi_keys = equi_key_sqls;
            join_info.residual_sql = residual_sql;
            join_info.emit_unmatched_probe = emit_unmatched_probe;
            join_info.emit_unmatched_build = emit_unmatched_build;

            if (swapped) {
                join_info.probe_side = scope.tables[1].name;
                join_info.build_side = scope.tables[0].name;

                auto probe_child = std::move(table_scans[1]);
                auto build_child = std::move(current_op);

                size_t probe_w = scope.tables[1].tuple_width;
                size_t build_w = scope.tables[0].tuple_width;
                size_t written_left_w = scope.tables[0].tuple_width;
                size_t written_right_w = scope.tables[1].tuple_width;

                current_op = std::make_unique<HashJoinOperator>(
                    std::move(probe_child),
                    std::move(build_child),
                    j.type,
                    /*swapped=*/true,
                    emit_unmatched_probe,
                    emit_unmatched_build,
                    std::move(build_keys),
                    std::move(probe_keys),
                    std::move(build_affs),
                    std::move(probe_affs),
                    std::move(residual_pred),
                    probe_w,
                    build_w,
                    kept_build_slots,
                    written_left_w,
                    written_right_w,
                    is_nested_loop,
                    options.join_stats
                );
            } else {
                join_info.probe_side = (i == 1 ? scope.tables[0].name : "join_" + std::to_string(i - 1));
                join_info.build_side = scope.tables[i].name;

                auto probe_child = std::move(current_op);
                auto build_child = std::move(table_scans[i]);

                size_t probe_w = current_probe_width;
                size_t build_w = scope.tables[i].tuple_width;
                size_t written_left_w = probe_w;
                size_t written_right_w = build_w;

                current_op = std::make_unique<HashJoinOperator>(
                    std::move(probe_child),
                    std::move(build_child),
                    j.type,
                    /*swapped=*/false,
                    emit_unmatched_probe,
                    emit_unmatched_build,
                    std::move(probe_keys),
                    std::move(build_keys),
                    std::move(probe_affs),
                    std::move(build_affs),
                    std::move(residual_pred),
                    probe_w,
                    build_w,
                    kept_build_slots,
                    written_left_w,
                    written_right_w,
                    is_nested_loop,
                    options.join_stats
                );
            }

            current_probe_width += scope.tables[i].tuple_width;
            plan.join_plans.push_back(std::move(join_info));
        }

        // Attach unpushed WHERE filter if present
        if (!unpushed_where.empty()) {
            std::vector<std::unique_ptr<BoundExpr>> unpushed_bounds;
            std::string unpushed_sql;
            for (const auto* c : unpushed_where) {
                unpushed_bounds.push_back(bindExpr(c, scope, plan.literal_arena));
                if (!unpushed_sql.empty()) unpushed_sql += " AND ";
                unpushed_sql += c->toSql();
            }
            plan.has_filter = true;
            plan.where_sql = unpushed_sql;
            auto bound_unpushed = combineBoundConjunction(std::move(unpushed_bounds));
            current_op = std::make_unique<FilterOperator>(std::move(current_op), std::move(bound_unpushed));
        } else {
            plan.has_filter = false;
        }

        // Project -> [Distinct] -> [Sort] -> [Limit]
        current_op = std::make_unique<ProjectOperator>(
            std::move(current_op),
            std::move(bound_selects),
            plan.output_headers
        );

        if (stmt.distinct) {
            plan.has_distinct = true;
            current_op = std::make_unique<DistinctOperator>(std::move(current_op), plan.visible_width, options.max_groups);
        }

        if (!stmt.order_by.empty()) {
            plan.has_sort = true;
            long long top_n = -1;
            if (stmt.limit.has_value()) {
                long long lim = stmt.limit.value();
                long long off = stmt.offset.value_or(0);
                if (lim <= 0) {
                    top_n = 0;
                } else {
                    if (off > 0 && lim > std::numeric_limits<long long>::max() - off) {
                        top_n = std::numeric_limits<long long>::max();
                    } else {
                        top_n = lim + off;
                    }
                    if (top_n > 100000) {
                        top_n = -1;
                    }
                }
            }
            plan.sort_is_top_n = (top_n >= 0);
            plan.sort_top_n = top_n;
            size_t mem_bytes = options.memory_limit_mb * 1024ULL * 1024ULL;
            current_op = std::make_unique<SortOperator>(
                std::move(current_op),
                std::move(sort_terms),
                top_n,
                mem_bytes,
                options.tmp_dir,
                options.sort_stats,
                options.sort_fan_in
            );
        }

        if (stmt.limit.has_value() || stmt.offset.has_value()) {
            plan.has_limit = true;
            plan.limit = stmt.limit.value_or(-1);
            plan.offset = stmt.offset.value_or(0);
            current_op = std::make_unique<LimitOperator>(std::move(current_op), plan.limit, plan.offset);
        }

        plan.root = std::move(current_op);
        return plan;
    }

    // -------------------------------------------------------------------------
    // Aggregate query path
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
        bound_group_by.push_back(bindExpr(gb.get(), scope, plan.literal_arena));
        plan.group_by_sqls.push_back(gb->toSql());
        collectTableNeededColumns(gb.get(), scope, table_needed_cols);
    }

    std::vector<AggSpec> agg_specs;

    // Rewrite SELECT items and determine headers
    std::vector<std::unique_ptr<BoundExpr>> bound_selects;
    for (const auto& item : stmt.items) {
        std::string header;
        if (!item.alias.empty()) {
            header = item.alias;
        } else if (item.expr->kind == ExprKind::COLUMN_REF) {
            header = item.expr->col_name;
        } else {
            header = item.expr->toSql();
        }
        plan.output_headers.push_back(header);
        plan.select_sqls.push_back(item.expr->toSql());
        bound_selects.push_back(rewritePostAggregateExpr(item.expr.get(), scope, bound_group_by, agg_specs, plan));
    }

    size_t N = bound_selects.size();
    plan.visible_width = N;

    // Rewrite HAVING if present
    std::unique_ptr<BoundExpr> bound_having = nullptr;
    if (stmt.having) {
        plan.has_having = true;
        plan.having_sql = stmt.having->toSql();
        bound_having = rewritePostAggregateExpr(stmt.having.get(), scope, bound_group_by, agg_specs, plan);
    }

    // Process ORDER BY terms for aggregate query
    std::vector<SortTerm> sort_terms;
    std::vector<std::unique_ptr<BoundExpr>> hidden_exprs;

    for (const auto& ob_item : stmt.order_by) {
        const Expr* expr = ob_item.expr.get();
        int found_slot = -1;

        // Step 1: Integer literal ordinal
        if (expr->kind == ExprKind::LITERAL_INT) {
            long long k = expr->int_val;
            if (k < 1 || k > static_cast<long long>(N)) {
                std::cerr << "Error: ORDER BY term out of range\n";
                std::exit(2);
            }
            found_slot = static_cast<int>(k - 1);
        }

        // Step 2: Bare unqualified identifier matching select item alias
        if (found_slot < 0 && expr->kind == ExprKind::COLUMN_REF && expr->table_name.empty()) {
            std::string col_lower = toLower(expr->col_name);
            for (size_t i = 0; i < stmt.items.size(); ++i) {
                if (!stmt.items[i].alias.empty() && toLower(stmt.items[i].alias) == col_lower) {
                    found_slot = static_cast<int>(i);
                    break;
                }
            }
        }

        // Step 3: Rewrite with rewritePostAggregateExpr (adds aggregates to agg_specs)
        if (found_slot < 0) {
            auto bound_ob = rewritePostAggregateExpr(expr, scope, bound_group_by, agg_specs, plan);
            for (size_t s = 0; s < N; ++s) {
                if (boundExprsEqual(bound_ob.get(), bound_selects[s].get())) {
                    found_slot = static_cast<int>(s);
                    break;
                }
            }
            if (found_slot < 0) {
                if (stmt.distinct) {
                    std::cerr << "Error: for SELECT DISTINCT, ORDER BY terms must appear in the select list\n";
                    std::exit(2);
                }
                for (size_t h = 0; h < hidden_exprs.size(); ++h) {
                    if (boundExprsEqual(bound_ob.get(), hidden_exprs[h].get())) {
                        found_slot = static_cast<int>(N + h);
                        break;
                    }
                }
                if (found_slot < 0) {
                    found_slot = static_cast<int>(N + hidden_exprs.size());
                    hidden_exprs.push_back(std::move(bound_ob));
                }
            }
        }

        sort_terms.push_back({static_cast<size_t>(found_slot), ob_item.descending});
        plan.sort_terms.push_back({expr->toSql(), ob_item.descending});
    }

    if (stmt.distinct) {
        assert(hidden_exprs.empty());
    }

    plan.hidden_columns_count = hidden_exprs.size();
    for (size_t h = 0; h < hidden_exprs.size(); ++h) {
        bound_selects.push_back(std::move(hidden_exprs[h]));
        plan.output_headers.push_back("hidden_" + std::to_string(h));
    }

    // Collect needed columns from AggSpec arguments (including those from ORDER BY)
    for (const auto& spec : agg_specs) {
        if (spec.arg) {
            collectBoundTableNeededColumns(spec.arg.get(), scope, table_needed_cols);
        }
    }

    // Collect needed columns from all ON clauses
    for (const auto& j : stmt.joins) {
        collectTableNeededColumns(j.on_expr.get(), scope, table_needed_cols);
    }

    // Finalize needed columns per table
    for (size_t t = 0; t < M; ++t) {
        std::sort(table_needed_cols[t].begin(), table_needed_cols[t].end());
        table_needed_cols[t].erase(std::unique(table_needed_cols[t].begin(), table_needed_cols[t].end()), table_needed_cols[t].end());
    }
    plan.needed_columns = table_needed_cols[0];
    for (size_t c : table_needed_cols[0]) {
        plan.needed_column_names.push_back(scope.tables[0].schema[c].name);
    }

    // Construct base table scans
    std::vector<std::unique_ptr<Operator>> table_scans;
    for (size_t t = 0; t < M; ++t) {
        const auto& ts = scope.tables[t];
        std::vector<ColType> col_types;
        std::vector<std::string> all_col_names;
        for (const auto& col : ts.schema) {
            col_types.push_back(col.type);
            all_col_names.push_back(col.name);
        }

        std::unique_ptr<Operator> scan_op = std::make_unique<ScanOperator>(
            ts.csv_path,
            table_needed_cols[t],
            col_types,
            all_col_names
        );

        if (!pushed_where[t].empty()) {
            MultiTableScope local_scope;
            TableScope copy = ts;
            copy.tuple_offset = 0;
            local_scope.tables.push_back(std::move(copy));
            local_scope.total_width = ts.schema.size();

            std::vector<std::unique_ptr<BoundExpr>> pushed_bounds;
            for (const auto* c : pushed_where[t]) {
                pushed_bounds.push_back(bindExpr(c, local_scope, plan.literal_arena));
            }
            auto bound_pushed = combineBoundConjunction(std::move(pushed_bounds));
            scan_op = std::make_unique<FilterOperator>(std::move(scan_op), std::move(bound_pushed));
        }
        table_scans.push_back(std::move(scan_op));
    }

    // Assemble join tree
    std::unique_ptr<Operator> current_op = std::move(table_scans[0]);
    size_t current_probe_width = scope.tables[0].tuple_width;

    for (size_t i = 1; i < M; ++i) {
        const auto& j = stmt.joins[i - 1];
        std::vector<const Expr*> on_conjuncts;
        splitConjunction(j.on_expr.get(), on_conjuncts);

        MultiTableScope scope_probe;
        for (size_t k = 0; k < i; ++k) {
            TableScope copy = scope.tables[k];
            scope_probe.tables.push_back(std::move(copy));
            scope_probe.total_width += copy.tuple_width;
        }

        MultiTableScope local_scope_i;
        TableScope copy_i = scope.tables[i];
        copy_i.tuple_offset = 0;
        local_scope_i.tables.push_back(std::move(copy_i));
        local_scope_i.total_width = copy_i.tuple_width;

        MultiTableScope scope_up_to_i;
        for (size_t k = 0; k <= i; ++k) {
            TableScope copy = scope.tables[k];
            scope_up_to_i.tables.push_back(std::move(copy));
            scope_up_to_i.total_width += copy.tuple_width;
        }

        std::vector<std::unique_ptr<BoundExpr>> probe_keys;
        std::vector<std::unique_ptr<BoundExpr>> build_keys;
        std::vector<Affinity> probe_affs;
        std::vector<Affinity> build_affs;
        std::vector<std::pair<std::string, std::string>> equi_key_sqls;
        std::vector<const Expr*> residual_conjuncts;

        for (const auto* c : on_conjuncts) {
            bool is_equi = false;
            if (c->kind == ExprKind::BINARY_OP && c->op == "=") {
                std::set<size_t> left_tables;
                std::set<size_t> right_tables;
                collectReferencedTables(c->left.get(), scope_up_to_i, left_tables);
                collectReferencedTables(c->right.get(), scope_up_to_i, right_tables);

                if (!left_tables.empty() && !right_tables.empty()) {
                    bool left_is_probe = (*left_tables.rbegin() < i);
                    bool right_is_build = (right_tables.size() == 1 && *right_tables.begin() == i);

                    bool right_is_probe = (*right_tables.rbegin() < i);
                    bool left_is_build = (left_tables.size() == 1 && *left_tables.begin() == i);

                    if (left_is_probe && right_is_build) {
                        auto p_key = bindExpr(c->left.get(), scope_probe, plan.literal_arena);
                        auto b_key = bindExpr(c->right.get(), local_scope_i, plan.literal_arena);
                        probe_affs.push_back(p_key->affinity);
                        build_affs.push_back(b_key->affinity);
                        probe_keys.push_back(std::move(p_key));
                        build_keys.push_back(std::move(b_key));
                        equi_key_sqls.push_back({c->left->toSql(), c->right->toSql()});
                        is_equi = true;
                    } else if (right_is_probe && left_is_build) {
                        auto p_key = bindExpr(c->right.get(), scope_probe, plan.literal_arena);
                        auto b_key = bindExpr(c->left.get(), local_scope_i, plan.literal_arena);
                        probe_affs.push_back(p_key->affinity);
                        build_affs.push_back(b_key->affinity);
                        probe_keys.push_back(std::move(p_key));
                        build_keys.push_back(std::move(b_key));
                        equi_key_sqls.push_back({c->right->toSql(), c->left->toSql()});
                        is_equi = true;
                    }
                }
            }
            if (!is_equi) {
                residual_conjuncts.push_back(c);
            }
        }

        std::unique_ptr<BoundExpr> residual_pred = nullptr;
        std::string residual_sql;
        if (!residual_conjuncts.empty()) {
            std::vector<std::unique_ptr<BoundExpr>> bound_res;
            for (const auto* r : residual_conjuncts) {
                bound_res.push_back(bindExpr(r, scope_up_to_i, plan.literal_arena));
                if (!residual_sql.empty()) residual_sql += " AND ";
                residual_sql += r->toSql();
            }
            residual_pred = combineBoundConjunction(std::move(bound_res));
        }

        bool is_nested_loop = probe_keys.empty();

        // Build swap heuristic: if !options.no_join_swap && i == 1 && (INNER || RIGHT) && fileSize(table 0) < fileSize(table 1)
        bool swapped = false;
        if (!options.no_join_swap && i == 1 && (j.type == JoinType::INNER || j.type == JoinType::RIGHT)) {
            size_t sz0 = getFileSize(scope.tables[0].csv_path);
            size_t sz1 = getFileSize(scope.tables[1].csv_path);
            if (sz0 < sz1) {
                swapped = true;
            }
        }

        bool emit_unmatched_probe = false;
        bool emit_unmatched_build = false;
        if (j.type == JoinType::INNER) {
            emit_unmatched_probe = false;
            emit_unmatched_build = false;
        } else if (j.type == JoinType::LEFT) {
            emit_unmatched_probe = true;
            emit_unmatched_build = false;
        } else if (j.type == JoinType::RIGHT) {
            if (swapped) {
                // Swapped RIGHT: probe is table 1 (R, preserved), build is table 0 (L)
                emit_unmatched_probe = true;
                emit_unmatched_build = false;
            } else {
                // Default RIGHT: probe is L, build is R (preserved)
                emit_unmatched_probe = false;
                emit_unmatched_build = true;
            }
        } else if (j.type == JoinType::FULL) {
            emit_unmatched_probe = true;
            emit_unmatched_build = true;
            swapped = false; // never swapped
        }

        std::vector<size_t> kept_build_slots;
        size_t build_table_idx = swapped ? 0 : i;

        std::set<size_t> kept_set;
        for (size_t c : table_needed_cols[build_table_idx]) {
            kept_set.insert(c);
        }
        for (size_t slot : kept_set) {
            kept_build_slots.push_back(slot);
        }
        std::sort(kept_build_slots.begin(), kept_build_slots.end());

        Plan::JoinPlanInfo join_info;
        switch (j.type) {
            case JoinType::INNER: join_info.join_type = "INNER"; break;
            case JoinType::LEFT:  join_info.join_type = "LEFT";  break;
            case JoinType::RIGHT: join_info.join_type = "RIGHT"; break;
            case JoinType::FULL:  join_info.join_type = "FULL";  break;
        }
        join_info.swapped = swapped;
        join_info.is_nested_loop = is_nested_loop;
        join_info.equi_keys = equi_key_sqls;
        join_info.residual_sql = residual_sql;
        join_info.emit_unmatched_probe = emit_unmatched_probe;
        join_info.emit_unmatched_build = emit_unmatched_build;

        if (swapped) {
            join_info.probe_side = scope.tables[1].name;
            join_info.build_side = scope.tables[0].name;

            auto probe_child = std::move(table_scans[1]);
            auto build_child = std::move(current_op);

            size_t probe_w = scope.tables[1].tuple_width;
            size_t build_w = scope.tables[0].tuple_width;
            size_t written_left_w = scope.tables[0].tuple_width;
            size_t written_right_w = scope.tables[1].tuple_width;

            current_op = std::make_unique<HashJoinOperator>(
                std::move(probe_child),
                std::move(build_child),
                j.type,
                /*swapped=*/true,
                emit_unmatched_probe,
                emit_unmatched_build,
                std::move(build_keys),
                std::move(probe_keys),
                std::move(build_affs),
                std::move(probe_affs),
                std::move(residual_pred),
                probe_w,
                build_w,
                kept_build_slots,
                written_left_w,
                written_right_w,
                is_nested_loop,
                options.join_stats
            );
        } else {
            join_info.probe_side = (i == 1 ? scope.tables[0].name : "join_" + std::to_string(i - 1));
            join_info.build_side = scope.tables[i].name;

            auto probe_child = std::move(current_op);
            auto build_child = std::move(table_scans[i]);

            size_t probe_w = current_probe_width;
            size_t build_w = scope.tables[i].tuple_width;
            size_t written_left_w = probe_w;
            size_t written_right_w = build_w;

            current_op = std::make_unique<HashJoinOperator>(
                std::move(probe_child),
                std::move(build_child),
                j.type,
                /*swapped=*/false,
                emit_unmatched_probe,
                emit_unmatched_build,
                std::move(probe_keys),
                std::move(build_keys),
                std::move(probe_affs),
                std::move(build_affs),
                std::move(residual_pred),
                probe_w,
                build_w,
                kept_build_slots,
                written_left_w,
                written_right_w,
                is_nested_loop,
                options.join_stats
            );
        }

        current_probe_width += scope.tables[i].tuple_width;
        plan.join_plans.push_back(std::move(join_info));
    }

    // Attach unpushed WHERE filter if present
    if (!unpushed_where.empty()) {
        std::vector<std::unique_ptr<BoundExpr>> unpushed_bounds;
        std::string unpushed_sql;
        for (const auto* c : unpushed_where) {
            unpushed_bounds.push_back(bindExpr(c, scope, plan.literal_arena));
            if (!unpushed_sql.empty()) unpushed_sql += " AND ";
            unpushed_sql += c->toSql();
        }
        plan.has_filter = true;
        plan.where_sql = unpushed_sql;
        auto bound_unpushed = combineBoundConjunction(std::move(unpushed_bounds));
        current_op = std::make_unique<FilterOperator>(std::move(current_op), std::move(bound_unpushed));
    } else {
        plan.has_filter = false;
    }

    // Assemble operator pipeline:
    // JoinTree -> [Filter WHERE] -> Aggregate -> [Filter HAVING] -> Project -> [Distinct] -> [Sort] -> [Limit]
    current_op = std::make_unique<AggregateOperator>(
        std::move(current_op),
        std::move(bound_group_by),
        std::move(agg_specs),
        options.max_groups
    );

    if (bound_having) {
        current_op = std::make_unique<FilterOperator>(std::move(current_op), std::move(bound_having));
    }

    current_op = std::make_unique<ProjectOperator>(
        std::move(current_op),
        std::move(bound_selects),
        plan.output_headers
    );

    if (stmt.distinct) {
        plan.has_distinct = true;
        current_op = std::make_unique<DistinctOperator>(std::move(current_op), plan.visible_width, options.max_groups);
    }

    if (!stmt.order_by.empty()) {
        plan.has_sort = true;
        long long top_n = -1;
        if (stmt.limit.has_value()) {
            long long lim = stmt.limit.value();
            long long off = stmt.offset.value_or(0);
            if (lim <= 0) {
                top_n = 0;
            } else {
                if (off > 0 && lim > std::numeric_limits<long long>::max() - off) {
                    top_n = std::numeric_limits<long long>::max();
                } else {
                    top_n = lim + off;
                }
                if (top_n > 100000) {
                    top_n = -1;
                }
            }
        }
        plan.sort_is_top_n = (top_n >= 0);
        plan.sort_top_n = top_n;
        size_t mem_bytes = options.memory_limit_mb * 1024ULL * 1024ULL;
        current_op = std::make_unique<SortOperator>(
            std::move(current_op),
            std::move(sort_terms),
            top_n,
            mem_bytes,
            options.tmp_dir,
            options.sort_stats,
            options.sort_fan_in
        );
    }

    if (stmt.limit.has_value() || stmt.offset.has_value()) {
        plan.has_limit = true;
        plan.limit = stmt.limit.value_or(-1);
        plan.offset = stmt.offset.value_or(0);
        current_op = std::make_unique<LimitOperator>(std::move(current_op), plan.limit, plan.offset);
    }

    plan.root = std::move(current_op);
    return plan;
}

void Plan::explain(FILE* out) const {
    if (join_plans.empty()) {
        std::fprintf(out, "Scan (table: %s, file: %s, needed_columns: [", table_name.c_str(), csv_path.c_str());
        for (size_t i = 0; i < needed_column_names.size(); ++i) {
            if (i > 0) std::fprintf(out, ", ");
            std::fprintf(out, "%s", needed_column_names[i].c_str());
        }
        std::fprintf(out, "])\n");
    } else {
        std::fprintf(out, "Joins (%zu joins):\n", join_plans.size());
        for (size_t j = 0; j < join_plans.size(); ++j) {
            const auto& jp = join_plans[j];
            std::fprintf(out, "  Join %zu (type: %s, probe: %s, build: %s, swapped: %s, mode: %s",
                         j, jp.join_type.c_str(), jp.probe_side.c_str(), jp.build_side.c_str(),
                         jp.swapped ? "yes" : "no",
                         jp.is_nested_loop ? "NESTED_LOOP" : "HASH");
            if (!jp.equi_keys.empty()) {
                std::fprintf(out, ", equi_keys: [");
                for (size_t k = 0; k < jp.equi_keys.size(); ++k) {
                    if (k > 0) std::fprintf(out, ", ");
                    std::fprintf(out, "%s = %s", jp.equi_keys[k].first.c_str(), jp.equi_keys[k].second.c_str());
                }
                std::fprintf(out, "]");
            }
            if (!jp.residual_sql.empty()) {
                std::fprintf(out, ", residual: %s", jp.residual_sql.c_str());
            }
            std::fprintf(out, ", emit_unmatched_probe: %s, emit_unmatched_build: %s)\n",
                         jp.emit_unmatched_probe ? "yes" : "no",
                         jp.emit_unmatched_build ? "yes" : "no");
        }
    }

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
    std::fprintf(out, "], visible_width: %zu, hidden_columns: %zu)\n", visible_width, hidden_columns_count);
    indent += 2;

    if (has_distinct) {
        std::fprintf(out, "%*sDistinct\n", indent, "");
        indent += 2;
    }

    if (has_sort) {
        std::fprintf(out, "%*sSort (terms: [", indent, "");
        for (size_t i = 0; i < sort_terms.size(); ++i) {
            if (i > 0) std::fprintf(out, ", ");
            std::fprintf(out, "%s %s", sort_terms[i].sql.c_str(), sort_terms[i].descending ? "DESC" : "ASC");
        }
        if (sort_is_top_n) {
            std::fprintf(out, "], mode: TOP-N (n: %lld))\n", sort_top_n);
        } else {
            std::fprintf(out, "], mode: FULL)\n");
        }
        indent += 2;
    }

    if (has_limit) {
        std::fprintf(out, "%*sLimit (limit: %lld, offset: %lld)\n", indent, "", limit, offset);
    }
}

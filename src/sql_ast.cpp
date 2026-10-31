#include "sql_ast.h"

#include <iomanip>

static std::string escapeSqlString(const std::string& str) {
    std::string res;
    res.reserve(str.size() + 2);
    for (char c : str) {
        if (c == '\'') {
            res += "''";
        } else {
            res += c;
        }
    }
    return res;
}

static std::string quoteIdentIfNeeded(const std::string& str) {
    if (str.empty()) return str;
    bool needs_quote = false;
    if (std::isdigit(static_cast<unsigned char>(str[0]))) {
        needs_quote = true;
    }
    for (char c : str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return str;

    std::string res = "\"";
    for (char c : str) {
        if (c == '"') {
            res += "\"\"";
        } else {
            res += c;
        }
    }
    res += "\"";
    return res;
}

std::string Expr::toSExpr() const {
    std::ostringstream oss;
    switch (kind) {
        case ExprKind::LITERAL_INT:
            oss << "(int " << int_val << ")";
            break;
        case ExprKind::LITERAL_FLOAT:
            oss << "(float " << str_val << ")";
            break;
        case ExprKind::LITERAL_STRING:
            oss << "(string '" << escapeSqlString(str_val) << "')";
            break;
        case ExprKind::LITERAL_NULL:
            oss << "(null)";
            break;
        case ExprKind::COLUMN_REF:
            if (!table_name.empty()) {
                oss << "(col " << table_name << " " << col_name << ")";
            } else {
                oss << "(col " << col_name << ")";
            }
            break;
        case ExprKind::STAR:
            if (!table_name.empty()) {
                oss << "(col " << table_name << " *)";
            } else {
                oss << "*";
            }
            break;
        case ExprKind::UNARY_OP:
            oss << "(" << op << " " << child->toSExpr() << ")";
            break;
        case ExprKind::BINARY_OP: {
            std::string canonical_op = op;
            // Print <> as != in AST so both spellings yield identical canonical S-expressions
            if (canonical_op == "<>") {
                canonical_op = "!=";
            }
            oss << "(" << canonical_op << " " << left->toSExpr() << " " << right->toSExpr() << ")";
            break;
        }
        case ExprKind::IS_NULL:
            // is_null <expr> true for IS NULL; is_null <expr> false for IS NOT NULL
            oss << "(is_null " << child->toSExpr() << " " << (is_not ? "false" : "true") << ")";
            break;
        case ExprKind::BETWEEN: {
            std::string inner = "(between " + between_expr->toSExpr() + " " +
                                between_low->toSExpr() + " " +
                                between_high->toSExpr() + ")";
            if (is_not) {
                oss << "(not " << inner << ")";
            } else {
                oss << inner;
            }
            break;
        }
        case ExprKind::IN_LIST: {
            std::ostringstream inner;
            inner << "(in " << in_expr->toSExpr() << " (";
            for (size_t i = 0; i < in_list.size(); ++i) {
                if (i > 0) inner << " ";
                inner << in_list[i]->toSExpr();
            }
            inner << "))";
            if (is_not) {
                oss << "(not " << inner.str() << ")";
            } else {
                oss << inner.str();
            }
            break;
        }
        case ExprKind::LIKE: {
            std::string inner = "(like " + like_expr->toSExpr() + " " + like_pattern->toSExpr() + ")";
            if (is_not) {
                oss << "(not " << inner << ")";
            } else {
                oss << inner;
            }
            break;
        }
        case ExprKind::FUNCTION_CALL: {
            oss << "(agg " << func_name;
            if (func_star) {
                oss << " *)";
            } else if (func_distinct) {
                oss << " (distinct " << func_args[0]->toSExpr() << "))";
            } else {
                for (const auto& arg : func_args) {
                    oss << " " << arg->toSExpr();
                }
                oss << ")";
            }
            break;
        }
    }
    return oss.str();
}

std::string Expr::toSql() const {
    std::ostringstream oss;
    switch (kind) {
        case ExprKind::LITERAL_INT:
            oss << int_val;
            break;
        case ExprKind::LITERAL_FLOAT:
            oss << str_val;
            break;
        case ExprKind::LITERAL_STRING:
            oss << "'" << escapeSqlString(str_val) << "'";
            break;
        case ExprKind::LITERAL_NULL:
            oss << "NULL";
            break;
        case ExprKind::COLUMN_REF:
            if (!table_name.empty()) {
                oss << quoteIdentIfNeeded(table_name) << "." << quoteIdentIfNeeded(col_name);
            } else {
                oss << quoteIdentIfNeeded(col_name);
            }
            break;
        case ExprKind::STAR:
            if (!table_name.empty()) {
                oss << quoteIdentIfNeeded(table_name) << ".*";
            } else {
                oss << "*";
            }
            break;
        case ExprKind::UNARY_OP:
            if (op == "not") {
                oss << "(NOT " << child->toSql() << ")";
            } else {
                oss << "(" << op << " " << child->toSql() << ")";
            }
            break;
        case ExprKind::BINARY_OP: {
            std::string sql_op = op;
            if (sql_op == "and") sql_op = "AND";
            else if (sql_op == "or") sql_op = "OR";
            oss << "(" << left->toSql() << " " << sql_op << " " << right->toSql() << ")";
            break;
        }
        case ExprKind::IS_NULL:
            if (is_not) {
                oss << "(" << child->toSql() << " IS NOT NULL)";
            } else {
                oss << "(" << child->toSql() << " IS NULL)";
            }
            break;
        case ExprKind::BETWEEN:
            if (is_not) {
                oss << "(" << between_expr->toSql() << " NOT BETWEEN "
                    << between_low->toSql() << " AND " << between_high->toSql() << ")";
            } else {
                oss << "(" << between_expr->toSql() << " BETWEEN "
                    << between_low->toSql() << " AND " << between_high->toSql() << ")";
            }
            break;
        case ExprKind::IN_LIST: {
            oss << "(" << in_expr->toSql();
            if (is_not) {
                oss << " NOT IN (";
            } else {
                oss << " IN (";
            }
            for (size_t i = 0; i < in_list.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << in_list[i]->toSql();
            }
            oss << "))";
            break;
        }
        case ExprKind::LIKE:
            if (is_not) {
                oss << "(" << like_expr->toSql() << " NOT LIKE " << like_pattern->toSql() << ")";
            } else {
                oss << "(" << like_expr->toSql() << " LIKE " << like_pattern->toSql() << ")";
            }
            break;
        case ExprKind::FUNCTION_CALL:
            oss << func_name << "(";
            if (func_star) {
                oss << "*";
            } else if (func_distinct) {
                oss << "DISTINCT " << func_args[0]->toSql();
            } else {
                for (size_t i = 0; i < func_args.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << func_args[i]->toSql();
                }
            }
            oss << ")";
            break;
    }
    return oss.str();
}

std::string SelectItem::toSExpr() const {
    std::string inner = expr->toSExpr();
    if (!alias.empty()) {
        return "(" + inner + " as " + alias + ")";
    }
    return inner;
}

std::string SelectItem::toSql() const {
    std::string res = expr->toSql();
    if (!alias.empty()) {
        res += " AS " + quoteIdentIfNeeded(alias);
    }
    return res;
}

std::string JoinClause::toSExpr() const {
    std::ostringstream oss;
    oss << "(join ";
    switch (type) {
        case JoinType::INNER: oss << "inner"; break;
        case JoinType::LEFT: oss << "left"; break;
        case JoinType::RIGHT: oss << "right"; break;
        case JoinType::FULL: oss << "full"; break;
    }
    oss << " " << table.name;
    if (!table.alias.empty()) {
        oss << " as " << table.alias;
    }
    oss << " (on " << on_expr->toSExpr() << "))";
    return oss.str();
}

std::string JoinClause::toSql() const {
    std::ostringstream oss;
    switch (type) {
        case JoinType::INNER: oss << "INNER JOIN "; break;
        case JoinType::LEFT: oss << "LEFT JOIN "; break;
        case JoinType::RIGHT: oss << "RIGHT JOIN "; break;
        case JoinType::FULL: oss << "FULL JOIN "; break;
    }
    oss << quoteIdentIfNeeded(table.name);
    if (!table.alias.empty()) {
        oss << " AS " << quoteIdentIfNeeded(table.alias);
    }
    oss << " ON " << on_expr->toSql();
    return oss.str();
}

std::string OrderByItem::toSExpr() const {
    return "(" + expr->toSExpr() + (descending ? " desc)" : " asc)");
}

std::string OrderByItem::toSql() const {
    return expr->toSql() + (descending ? " DESC" : " ASC");
}

std::string SelectStmt::toSExpr() const {
    std::ostringstream oss;
    oss << "(select (distinct " << (distinct ? "true" : "false") << ")\n";

    // items
    oss << "  (items";
    for (const auto& item : items) {
        oss << " " << item.toSExpr();
    }
    oss << ")\n";

    // from
    oss << "  (from " << from_table.name;
    if (!from_table.alias.empty()) {
        oss << " as " << from_table.alias;
    }
    oss << ")";

    // joins
    if (!joins.empty()) {
        oss << "\n  (joins";
        for (const auto& j : joins) {
            oss << " " << j.toSExpr();
        }
        oss << ")";
    }

    // where
    if (where_clause) {
        oss << "\n  (where " << where_clause->toSExpr() << ")";
    }

    // group_by
    if (!group_by.empty()) {
        oss << "\n  (group_by";
        for (const auto& gb : group_by) {
            oss << " " << gb->toSExpr();
        }
        oss << ")";
    }

    // having
    if (having) {
        oss << "\n  (having " << having->toSExpr() << ")";
    }

    // order_by
    if (!order_by.empty()) {
        oss << "\n  (order_by (";
        for (size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) oss << " ";
            oss << order_by[i].toSExpr();
        }
        oss << "))";
    }

    // limit
    if (limit.has_value()) {
        oss << "\n  (limit " << limit.value() << ")";
    }

    // offset
    if (offset.has_value()) {
        oss << "\n  (offset " << offset.value() << ")";
    }

    oss << ")";
    return oss.str();
}

std::string SelectStmt::toSql() const {
    std::ostringstream oss;
    oss << "SELECT ";
    if (distinct) {
        oss << "DISTINCT ";
    }

    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << items[i].toSql();
    }

    oss << " FROM " << quoteIdentIfNeeded(from_table.name);
    if (!from_table.alias.empty()) {
        oss << " AS " << quoteIdentIfNeeded(from_table.alias);
    }

    for (const auto& j : joins) {
        oss << " " << j.toSql();
    }

    if (where_clause) {
        oss << " WHERE " << where_clause->toSql();
    }

    if (!group_by.empty()) {
        oss << " GROUP BY ";
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << group_by[i]->toSql();
        }
    }

    if (having) {
        oss << " HAVING " << having->toSql();
    }

    if (!order_by.empty()) {
        oss << " ORDER BY ";
        for (size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << order_by[i].toSql();
        }
    }

    if (limit.has_value()) {
        oss << " LIMIT " << limit.value();
    }

    if (offset.has_value()) {
        oss << " OFFSET " << offset.value();
    }

    return oss.str();
}

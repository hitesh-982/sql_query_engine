#ifndef SQL_AST_H
#define SQL_AST_H

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <sstream>

enum class ExprKind {
    LITERAL_INT,
    LITERAL_FLOAT,
    LITERAL_STRING,
    LITERAL_NULL,
    COLUMN_REF,
    STAR,
    UNARY_OP,
    BINARY_OP,
    IS_NULL,
    BETWEEN,
    IN_LIST,
    LIKE,
    FUNCTION_CALL
};

enum class JoinType {
    INNER,
    LEFT,
    RIGHT,
    FULL
};

struct TableRef {
    std::string name;
    std::string alias; // Empty if no alias
};

struct Expr {
    ExprKind kind;

    // Literals
    long long int_val = 0;
    double float_val = 0.0;
    std::string str_val; // For strings and float raw textual representation

    // Column ref or star prefix
    std::string table_name; // Empty if unqualified
    std::string col_name;

    // Unary op: "-", "not"
    std::string op;
    std::unique_ptr<Expr> child;

    // Binary op: "+", "-", "*", "/", "=", "!=", "<", "<=", ">", ">=", "and", "or"
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    // IS NULL (is_not = false means IS NULL; is_not = true means IS NOT NULL)
    bool is_not = false;

    // BETWEEN
    std::unique_ptr<Expr> between_expr;
    std::unique_ptr<Expr> between_low;
    std::unique_ptr<Expr> between_high;

    // IN list
    std::unique_ptr<Expr> in_expr;
    std::vector<std::unique_ptr<Expr>> in_list;

    // LIKE
    std::unique_ptr<Expr> like_expr;
    std::unique_ptr<Expr> like_pattern;

    // Functions
    std::string func_name;
    std::vector<std::unique_ptr<Expr>> func_args;
    bool func_distinct = false;
    bool func_star = false; // e.g. COUNT(*)

    // S-expression printer
    std::string toSExpr() const;

    // Canonical SQL generator
    std::string toSql() const;
};

struct SelectItem {
    std::unique_ptr<Expr> expr;
    std::string alias; // Empty if no alias

    std::string toSExpr() const;
    std::string toSql() const;
};

struct JoinClause {
    JoinType type;
    TableRef table;
    std::unique_ptr<Expr> on_expr;

    std::string toSExpr() const;
    std::string toSql() const;
};

struct OrderByItem {
    std::unique_ptr<Expr> expr;
    bool descending = false;

    std::string toSExpr() const;
    std::string toSql() const;
};

struct SelectStmt {
    bool distinct = false;
    std::vector<SelectItem> items;
    TableRef from_table;
    std::vector<JoinClause> joins;
    std::unique_ptr<Expr> where_clause;
    std::vector<std::unique_ptr<Expr>> group_by;
    std::unique_ptr<Expr> having;
    std::vector<OrderByItem> order_by;
    std::optional<long long> limit;
    std::optional<long long> offset;

    // S-expression canonical representation
    std::string toSExpr() const;

    // Canonical SQL representation for round-trip testing
    std::string toSql() const;
};

// Free function as required by spec
inline std::string toSql(const SelectStmt& stmt) {
    return stmt.toSql();
}

#endif // SQL_AST_H

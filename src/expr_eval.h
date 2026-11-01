#ifndef EXPR_EVAL_H
#define EXPR_EVAL_H

#include "value.h"
#include "operator.h"
#include "sql_ast.h"
#include "csv_scan.h"

#include <string>
#include <vector>
#include <memory>
#include <deque>

enum class Affinity {
    NONE,
    NUMERIC,
    TEXT
};

enum class BoundExprKind {
    LITERAL,
    COLUMN_REF,
    SLOT_REF,
    UNARY_OP,
    BINARY_OP,
    IS_NULL,
    BETWEEN,
    IN_LIST,
    LIKE
};

struct BoundExpr {
    BoundExprKind kind;
    Affinity affinity = Affinity::NONE;

    // LITERAL
    Value lit_val = makeNull();

    // COLUMN_REF
    size_t col_idx = 0;
    std::string col_name;

    // SLOT_REF (used by Project / Filter after aggregation)
    size_t slot_idx = 0;

    // UNARY_OP or BINARY_OP
    std::string op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;

    // IS_NULL, BETWEEN, IN_LIST, LIKE
    bool is_not = false;

    // BETWEEN
    std::unique_ptr<BoundExpr> between_low;
    std::unique_ptr<BoundExpr> between_high;

    // IN_LIST
    std::vector<std::unique_ptr<BoundExpr>> in_list;

    // LIKE
    std::unique_ptr<BoundExpr> like_pattern;
};

// Structural equality of two bound expression trees
bool boundExprsEqual(const BoundExpr* a, const BoundExpr* b);

// Deep copy of a bound expression tree
std::unique_ptr<BoundExpr> cloneBoundExpr(const BoundExpr* expr);

// Check if string is entirely numeric (optional spaces, integer or float)
bool isEntirelyNumeric(std::string_view s, Value& out_num);

// Applies SQLite comparison affinity normalization to a copy of a key value.
// Never modifies emitted row tuples.
Value normalizeKeyForAffinity(Value val, Affinity own_aff, Affinity other_aff, char* text_buf, size_t buf_size);

// TableScope and MultiTableScope for multi-table queries
struct TableScope {
    std::string name;
    std::string alias;
    std::vector<Column> schema;
    size_t tuple_offset = 0;
    size_t tuple_width = 0;
    std::string csv_path;
};

struct MultiTableScope {
    std::vector<TableScope> tables;
    size_t total_width = 0;
};

// Bind an AST Expr into a BoundExpr tree against a multi-table scope.
// - Resolves column names (case-insensitive).
// - Validates table qualifier (alias hides table name, unknown table -> exit 2).
// - Resolves unqualified columns across scope (unknown -> exit 2, ambiguous -> exit 2).
// - String literals are stored in the provided literal arena.
std::unique_ptr<BoundExpr> bindExpr(
    const Expr* ast_expr,
    const MultiTableScope& scope,
    std::deque<std::string>& literal_arena
);

// Backwards-compatible overload for single-table queries
inline std::unique_ptr<BoundExpr> bindExpr(
    const Expr* ast_expr,
    const TableRef& from_table,
    const std::vector<Column>& schema,
    std::deque<std::string>& literal_arena
) {
    MultiTableScope scope;
    TableScope ts;
    ts.name = from_table.name;
    ts.alias = from_table.alias;
    ts.schema = schema;
    ts.tuple_offset = 0;
    ts.tuple_width = schema.size();
    scope.tables.push_back(std::move(ts));
    scope.total_width = schema.size();
    return bindExpr(ast_expr, scope, literal_arena);
}

// Evaluate a BoundExpr against a Tuple.
// S-expression/three-valued logic matching SQLite.
Value evalExpr(const BoundExpr* expr, const Tuple& tuple);

// Case-insensitive ASCII lowercase
inline char asciiToLower(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c + 32);
    return c;
}

// Length in bytes of one UTF-8 codepoint starting with byte c
inline size_t utf8CharLen(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Iterative non-recursive LIKE matching supporting UTF-8 for '_'
bool likeMatch(std::string_view text, std::string_view pat);

#endif // EXPR_EVAL_H

#include "expr_eval.h"

#include <iostream>
#include <limits>
#include <cstdlib>

static inline std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) {
        c = asciiToLower(c);
    }
    return lower;
}

// Check if a string is entirely a well-formed integer or floating-point number
bool isEntirelyNumeric(std::string_view s, Value& out_num) {
    size_t idx = 0;
    while (idx < s.size() && (s[idx] == ' ' || s[idx] == '\t' || s[idx] == '\n' || s[idx] == '\r')) {
        idx++;
    }
    if (idx >= s.size()) {
        return false;
    }
    size_t start = idx;
    if (s[idx] == '+' || s[idx] == '-') {
        idx++;
    }
    size_t digits_before = 0;
    while (idx < s.size() && (s[idx] >= '0' && s[idx] <= '9')) {
        digits_before++;
        idx++;
    }
    size_t digits_after = 0;
    if (idx < s.size() && s[idx] == '.') {
        if (digits_before > 0 || (idx + 1 < s.size() && s[idx + 1] >= '0' && s[idx + 1] <= '9')) {
            idx++;
            while (idx < s.size() && (s[idx] >= '0' && s[idx] <= '9')) {
                digits_after++;
                idx++;
            }
        }
    }
    if (digits_before + digits_after == 0) {
        return false;
    }
    if (idx < s.size() && (s[idx] == 'e' || s[idx] == 'E')) {
        size_t exp_idx = idx + 1;
        if (exp_idx < s.size() && (s[exp_idx] == '+' || s[exp_idx] == '-')) {
            exp_idx++;
        }
        if (exp_idx < s.size() && (s[exp_idx] >= '0' && s[exp_idx] <= '9')) {
            idx = exp_idx;
            while (idx < s.size() && (s[idx] >= '0' && s[idx] <= '9')) {
                idx++;
            }
        } else {
            return false;
        }
    }
    // Check optional trailing spaces
    while (idx < s.size() && (s[idx] == ' ' || s[idx] == '\t' || s[idx] == '\n' || s[idx] == '\r')) {
        idx++;
    }
    if (idx != s.size()) {
        return false;
    }

    // Fully numeric string: convert to number
    Value raw_v = makeString(s.substr(start, idx - start));
    out_num = toNumber(raw_v);
    return true;
}

std::unique_ptr<BoundExpr> bindExpr(
    const Expr* ast_expr,
    const TableRef& from_table,
    const std::vector<Column>& schema,
    std::deque<std::string>& literal_arena
) {
    if (!ast_expr) return nullptr;

    auto bound = std::make_unique<BoundExpr>();

    switch (ast_expr->kind) {
        case ExprKind::LITERAL_INT:
            bound->kind = BoundExprKind::LITERAL;
            bound->lit_val = makeInt(ast_expr->int_val);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::LITERAL_FLOAT:
            bound->kind = BoundExprKind::LITERAL;
            bound->lit_val = makeFloat(ast_expr->float_val);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::LITERAL_STRING:
            bound->kind = BoundExprKind::LITERAL;
            literal_arena.emplace_back(ast_expr->str_val);
            bound->lit_val = makeString(literal_arena.back());
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::LITERAL_NULL:
            bound->kind = BoundExprKind::LITERAL;
            bound->lit_val = makeNull();
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::COLUMN_REF: {
            bound->kind = BoundExprKind::COLUMN_REF;
            // Validate table qualifier if present
            if (!ast_expr->table_name.empty()) {
                std::string prefix_lower = toLower(ast_expr->table_name);
                std::string table_lower = toLower(from_table.name);
                std::string alias_lower = toLower(from_table.alias);
                if (prefix_lower != table_lower && (alias_lower.empty() || prefix_lower != alias_lower)) {
                    std::cerr << "Error: unknown table qualifier '" << ast_expr->table_name << "'\n";
                    std::exit(2);
                }
            }

            std::string target_lower = toLower(ast_expr->col_name);
            int found_idx = -1;
            for (size_t c = 0; c < schema.size(); ++c) {
                if (toLower(schema[c].name) == target_lower) {
                    found_idx = static_cast<int>(c);
                    break;
                }
            }
            if (found_idx < 0) {
                std::cerr << "Error: unknown column '" << ast_expr->col_name << "'\n";
                std::exit(2);
            }

            bound->col_idx = static_cast<size_t>(found_idx);
            bound->col_name = schema[found_idx].name;
            if (schema[found_idx].type == TYPE_STRING) {
                bound->affinity = Affinity::TEXT;
            } else {
                bound->affinity = Affinity::NUMERIC;
            }
            return bound;
        }

        case ExprKind::UNARY_OP:
            bound->kind = BoundExprKind::UNARY_OP;
            bound->op = toLower(ast_expr->op);
            bound->left = bindExpr(ast_expr->child.get(), from_table, schema, literal_arena);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::BINARY_OP:
            bound->kind = BoundExprKind::BINARY_OP;
            bound->op = toLower(ast_expr->op);
            bound->left = bindExpr(ast_expr->left.get(), from_table, schema, literal_arena);
            bound->right = bindExpr(ast_expr->right.get(), from_table, schema, literal_arena);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::IS_NULL:
            bound->kind = BoundExprKind::IS_NULL;
            bound->is_not = ast_expr->is_not;
            bound->left = bindExpr(ast_expr->child.get(), from_table, schema, literal_arena);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::BETWEEN:
            bound->kind = BoundExprKind::BETWEEN;
            bound->is_not = ast_expr->is_not;
            bound->left = bindExpr(ast_expr->between_expr.get(), from_table, schema, literal_arena);
            bound->between_low = bindExpr(ast_expr->between_low.get(), from_table, schema, literal_arena);
            bound->between_high = bindExpr(ast_expr->between_high.get(), from_table, schema, literal_arena);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::IN_LIST:
            bound->kind = BoundExprKind::IN_LIST;
            bound->is_not = ast_expr->is_not;
            bound->left = bindExpr(ast_expr->in_expr.get(), from_table, schema, literal_arena);
            for (const auto& item : ast_expr->in_list) {
                bound->in_list.push_back(bindExpr(item.get(), from_table, schema, literal_arena));
            }
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::LIKE:
            bound->kind = BoundExprKind::LIKE;
            bound->is_not = ast_expr->is_not;
            bound->left = bindExpr(ast_expr->like_expr.get(), from_table, schema, literal_arena);
            bound->like_pattern = bindExpr(ast_expr->like_pattern.get(), from_table, schema, literal_arena);
            bound->affinity = Affinity::NONE;
            return bound;

        case ExprKind::FUNCTION_CALL:
            std::cerr << "not implemented yet: aggregate function: " << ast_expr->func_name << "\n";
            std::exit(3);

        case ExprKind::STAR:
            std::cerr << "Error: unexpected STAR in expression binding\n";
            std::exit(2);
    }
    return nullptr;
}

static Value evalThreeValuedAnd(Value l, Value r) {
    if (l.type != V_NULL && !isTruthy(l)) {
        return makeInt(0);
    }
    if (r.type != V_NULL && !isTruthy(r)) {
        return makeInt(0);
    }
    if (l.type == V_NULL || r.type == V_NULL) {
        return makeNull();
    }
    return makeInt(1);
}

static Value evalThreeValuedOr(Value l, Value r) {
    if (l.type != V_NULL && isTruthy(l)) {
        return makeInt(1);
    }
    if (r.type != V_NULL && isTruthy(r)) {
        return makeInt(1);
    }
    if (l.type == V_NULL || r.type == V_NULL) {
        return makeNull();
    }
    return makeInt(0);
}

static Value evalThreeValuedNot(Value v) {
    if (v.type == V_NULL) {
        return makeNull();
    }
    return makeInt(isTruthy(v) ? 0 : 1);
}

static Value evalComparison(Value l, Affinity l_aff, Value r, Affinity r_aff, const std::string& op) {
    if (l.type == V_NULL || r.type == V_NULL) {
        return makeNull();
    }

    // Step 1: Affinity rules
    char text_buf_l[64];
    char text_buf_r[64];

    if (l_aff == Affinity::NUMERIC && (r_aff == Affinity::TEXT || r_aff == Affinity::NONE)) {
        if (r.type == V_STRING) {
            Value converted;
            if (isEntirelyNumeric(r.s, converted)) {
                r = converted;
            }
        }
    } else if (r_aff == Affinity::NUMERIC && (l_aff == Affinity::TEXT || l_aff == Affinity::NONE)) {
        if (l.type == V_STRING) {
            Value converted;
            if (isEntirelyNumeric(l.s, converted)) {
                l = converted;
            }
        }
    } else if (l_aff == Affinity::TEXT && r_aff == Affinity::NONE) {
        if (r.type == V_INT || r.type == V_FLOAT) {
            size_t n = formatValueToBuffer(r, text_buf_r, sizeof(text_buf_r));
            r = makeString(std::string_view(text_buf_r, n));
        }
    } else if (r_aff == Affinity::TEXT && l_aff == Affinity::NONE) {
        if (l.type == V_INT || l.type == V_FLOAT) {
            size_t n = formatValueToBuffer(l, text_buf_l, sizeof(text_buf_l));
            l = makeString(std::string_view(text_buf_l, n));
        }
    }

    // Step 2: Compare values
    int cmp = compareValues(l, r);
    bool res = false;
    if (op == "=") res = (cmp == 0);
    else if (op == "!=" || op == "<>") res = (cmp != 0);
    else if (op == "<") res = (cmp < 0);
    else if (op == "<=") res = (cmp <= 0);
    else if (op == ">") res = (cmp > 0);
    else if (op == ">=") res = (cmp >= 0);

    return makeInt(res ? 1 : 0);
}

bool likeMatch(std::string_view text, std::string_view pat) {
    size_t t_len = text.size();
    size_t p_len = pat.size();
    size_t t = 0;
    size_t p = 0;
    size_t star_p = std::string_view::npos;
    size_t star_t = std::string_view::npos;

    while (t < t_len) {
        if (p < p_len && pat[p] == '%') {
            star_p = p;
            p++;
            star_t = t;
        } else if (p < p_len && pat[p] == '_') {
            size_t clen = utf8CharLen(static_cast<unsigned char>(text[t]));
            t += clen;
            p++;
        } else if (p < p_len && asciiToLower(pat[p]) == asciiToLower(text[t])) {
            t++;
            p++;
        } else if (star_p != std::string_view::npos) {
            if (star_t >= t_len) {
                return false;
            }
            star_t += utf8CharLen(static_cast<unsigned char>(text[star_t]));
            t = star_t;
            p = star_p + 1;
        } else {
            return false;
        }
    }

    while (p < p_len && pat[p] == '%') {
        p++;
    }

    return p == p_len;
}

Value evalExpr(const BoundExpr* expr, const Tuple& tuple) {
    if (!expr) return makeNull();

    switch (expr->kind) {
        case BoundExprKind::LITERAL:
            return expr->lit_val;

        case BoundExprKind::COLUMN_REF:
            if (expr->col_idx < tuple.values.size()) {
                return tuple.values[expr->col_idx];
            }
            return makeNull();

        case BoundExprKind::SLOT_REF:
            if (expr->slot_idx < tuple.values.size()) {
                return tuple.values[expr->slot_idx];
            }
            return makeNull();

        case BoundExprKind::IS_NULL: {
            Value v = evalExpr(expr->left.get(), tuple);
            bool is_null = (v.type == V_NULL);
            if (expr->is_not) is_null = !is_null;
            return makeInt(is_null ? 1 : 0);
        }

        case BoundExprKind::UNARY_OP: {
            if (expr->op == "not") {
                Value v = evalExpr(expr->left.get(), tuple);
                return evalThreeValuedNot(v);
            }
            if (expr->op == "-") {
                Value v = evalExpr(expr->left.get(), tuple);
                if (v.type == V_NULL) return makeNull();
                Value num = toNumber(v);
                if (num.type == V_FLOAT) {
                    return makeFloat(-num.d);
                }
                // Unary minus on LLONG_MIN promotes to double
                if (num.i == std::numeric_limits<long long>::min()) {
                    return makeFloat(-(double)std::numeric_limits<long long>::min());
                }
                return makeInt(-num.i);
            }
            return makeNull();
        }

        case BoundExprKind::BINARY_OP: {
            // Three-valued logic with short-circuiting
            if (expr->op == "and") {
                Value l = evalExpr(expr->left.get(), tuple);
                if (l.type != V_NULL && !isTruthy(l)) {
                    return makeInt(0);
                }
                Value r = evalExpr(expr->right.get(), tuple);
                return evalThreeValuedAnd(l, r);
            }
            if (expr->op == "or") {
                Value l = evalExpr(expr->left.get(), tuple);
                if (l.type != V_NULL && isTruthy(l)) {
                    return makeInt(1);
                }
                Value r = evalExpr(expr->right.get(), tuple);
                return evalThreeValuedOr(l, r);
            }

            // Comparisons
            if (expr->op == "=" || expr->op == "!=" || expr->op == "<>" ||
                expr->op == "<" || expr->op == "<=" || expr->op == ">" || expr->op == ">=") {
                Value l = evalExpr(expr->left.get(), tuple);
                Value r = evalExpr(expr->right.get(), tuple);
                return evalComparison(l, expr->left->affinity, r, expr->right->affinity, expr->op);
            }

            // Arithmetic (+, -, *, /)
            Value l_val = evalExpr(expr->left.get(), tuple);
            Value r_val = evalExpr(expr->right.get(), tuple);
            if (l_val.type == V_NULL || r_val.type == V_NULL) {
                return makeNull();
            }

            Value l_num = toNumber(l_val);
            Value r_num = toNumber(r_val);

            // Floating point arithmetic if either is float
            if (l_num.type == V_FLOAT || r_num.type == V_FLOAT) {
                double l_d = (l_num.type == V_FLOAT) ? l_num.d : static_cast<double>(l_num.i);
                double r_d = (r_num.type == V_FLOAT) ? r_num.d : static_cast<double>(r_num.i);

                if (expr->op == "/") {
                    if (r_d == 0.0) return makeNull();
                    return makeFloat(l_d / r_d);
                }
                if (expr->op == "+") return makeFloat(l_d + r_d);
                if (expr->op == "-") return makeFloat(l_d - r_d);
                if (expr->op == "*") return makeFloat(l_d * r_d);
                return makeNull();
            }

            // Integer arithmetic
            if (expr->op == "/") {
                if (r_num.i == 0) return makeNull();
                // Division overflow check (LLONG_MIN / -1)
                if (l_num.i == std::numeric_limits<long long>::min() && r_num.i == -1) {
                    return makeFloat((double)l_num.i / -1.0);
                }
                return makeInt(l_num.i / r_num.i); // truncates toward zero
            }
            if (expr->op == "+") {
                long long res;
                if (__builtin_add_overflow(l_num.i, r_num.i, &res)) {
                    return makeFloat(static_cast<double>(l_num.i) + static_cast<double>(r_num.i));
                }
                return makeInt(res);
            }
            if (expr->op == "-") {
                long long res;
                if (__builtin_sub_overflow(l_num.i, r_num.i, &res)) {
                    return makeFloat(static_cast<double>(l_num.i) - static_cast<double>(r_num.i));
                }
                return makeInt(res);
            }
            if (expr->op == "*") {
                long long res;
                if (__builtin_mul_overflow(l_num.i, r_num.i, &res)) {
                    return makeFloat(static_cast<double>(l_num.i) * static_cast<double>(r_num.i));
                }
                return makeInt(res);
            }
            return makeNull();
        }

        case BoundExprKind::BETWEEN: {
            Value v_x = evalExpr(expr->left.get(), tuple);
            Value v_low = evalExpr(expr->between_low.get(), tuple);
            Value v_high = evalExpr(expr->between_high.get(), tuple);

            Value c1 = evalComparison(v_x, expr->left->affinity, v_low, expr->between_low->affinity, ">=");
            Value c2 = evalComparison(v_x, expr->left->affinity, v_high, expr->between_high->affinity, "<=");
            Value res = evalThreeValuedAnd(c1, c2);
            if (expr->is_not) {
                res = evalThreeValuedNot(res);
            }
            return res;
        }

        case BoundExprKind::IN_LIST: {
            Value v_x = evalExpr(expr->left.get(), tuple);
            bool has_null = (v_x.type == V_NULL);

            for (const auto& item : expr->in_list) {
                Value v_item = evalExpr(item.get(), tuple);
                if (v_item.type == V_NULL) {
                    has_null = true;
                } else if (!has_null) {
                    Value cmp = evalComparison(v_x, expr->left->affinity, v_item, item->affinity, "=");
                    if (cmp.type == V_INT && cmp.i == 1) {
                        return makeInt(expr->is_not ? 0 : 1);
                    }
                }
            }

            if (has_null) {
                return makeNull();
            }
            return makeInt(expr->is_not ? 1 : 0);
        }

        case BoundExprKind::LIKE: {
            Value t_val = evalExpr(expr->left.get(), tuple);
            Value p_val = evalExpr(expr->like_pattern.get(), tuple);
            if (t_val.type == V_NULL || p_val.type == V_NULL) {
                return makeNull();
            }
            char t_buf[64];
            char p_buf[64];
            std::string_view t_view;
            if (t_val.type == V_STRING) {
                t_view = t_val.s;
            } else {
                size_t n = formatValueToBuffer(t_val, t_buf, sizeof(t_buf));
                t_view = std::string_view(t_buf, n);
            }
            std::string_view p_view;
            if (p_val.type == V_STRING) {
                p_view = p_val.s;
            } else {
                size_t n = formatValueToBuffer(p_val, p_buf, sizeof(p_buf));
                p_view = std::string_view(p_buf, n);
            }

            bool match = likeMatch(t_view, p_view);
            if (expr->is_not) match = !match;
            return makeInt(match ? 1 : 0);
        }
    }
    return makeNull();
}

bool boundExprsEqual(const BoundExpr* a, const BoundExpr* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case BoundExprKind::LITERAL: {
            if (a->lit_val.type != b->lit_val.type) return false;
            switch (a->lit_val.type) {
                case V_NULL: return true;
                case V_INT: return a->lit_val.i == b->lit_val.i;
                case V_FLOAT: {
                    if (std::isnan(a->lit_val.d) && std::isnan(b->lit_val.d)) return true;
                    return a->lit_val.d == b->lit_val.d;
                }
                case V_STRING: return a->lit_val.s == b->lit_val.s;
            }
            return false;
        }
        case BoundExprKind::COLUMN_REF:
            return a->col_idx == b->col_idx;
        case BoundExprKind::SLOT_REF:
            return a->slot_idx == b->slot_idx;
        case BoundExprKind::UNARY_OP:
            return a->op == b->op && boundExprsEqual(a->left.get(), b->left.get());
        case BoundExprKind::BINARY_OP:
            return a->op == b->op &&
                   boundExprsEqual(a->left.get(), b->left.get()) &&
                   boundExprsEqual(a->right.get(), b->right.get());
        case BoundExprKind::IS_NULL:
            return a->is_not == b->is_not && boundExprsEqual(a->left.get(), b->left.get());
        case BoundExprKind::BETWEEN:
            return a->is_not == b->is_not &&
                   boundExprsEqual(a->left.get(), b->left.get()) &&
                   boundExprsEqual(a->between_low.get(), b->between_low.get()) &&
                   boundExprsEqual(a->between_high.get(), b->between_high.get());
        case BoundExprKind::IN_LIST:
            if (a->is_not != b->is_not) return false;
            if (!boundExprsEqual(a->left.get(), b->left.get())) return false;
            if (a->in_list.size() != b->in_list.size()) return false;
            for (size_t i = 0; i < a->in_list.size(); ++i) {
                if (!boundExprsEqual(a->in_list[i].get(), b->in_list[i].get())) return false;
            }
            return true;
        case BoundExprKind::LIKE:
            return a->is_not == b->is_not &&
                   boundExprsEqual(a->left.get(), b->left.get()) &&
                   boundExprsEqual(a->like_pattern.get(), b->like_pattern.get());
    }
    return false;
}

std::unique_ptr<BoundExpr> cloneBoundExpr(const BoundExpr* expr) {
    if (!expr) return nullptr;
    auto copy = std::make_unique<BoundExpr>();
    copy->kind = expr->kind;
    copy->affinity = expr->affinity;
    copy->lit_val = expr->lit_val;
    copy->col_idx = expr->col_idx;
    copy->col_name = expr->col_name;
    copy->slot_idx = expr->slot_idx;
    copy->op = expr->op;
    copy->is_not = expr->is_not;

    if (expr->left) copy->left = cloneBoundExpr(expr->left.get());
    if (expr->right) copy->right = cloneBoundExpr(expr->right.get());
    if (expr->between_low) copy->between_low = cloneBoundExpr(expr->between_low.get());
    if (expr->between_high) copy->between_high = cloneBoundExpr(expr->between_high.get());
    for (const auto& item : expr->in_list) {
        copy->in_list.push_back(cloneBoundExpr(item.get()));
    }
    if (expr->like_pattern) copy->like_pattern = cloneBoundExpr(expr->like_pattern.get());
    return copy;
}


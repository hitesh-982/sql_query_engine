#include "sql_parser.h"

#include <sstream>
#include <iostream>

ParseError::ParseError(size_t l, size_t c, const std::string& msg, const std::string& line_text)
    : line(l), col(c), message(msg), sql_line(line_text) {
    std::ostringstream oss;
    oss << "Parse error at line " << line << ", column " << col << ": " << message << "\n";
    if (!sql_line.empty()) {
        oss << sql_line << "\n";
        for (size_t i = 1; i < col; ++i) {
            oss << " ";
        }
        oss << "^\n";
    }
    formatted = oss.str();
}

SqlParser::SqlParser(std::string_view sql)
    : sql_(sql), pos_(0), depth_(0) {
    try {
        SqlLexer lexer(sql);
        tokens_ = lexer.tokenize();
    } catch (const std::exception& e) {
        // If the lexer encounters an error (unterminated string/comment), wrap as ParseError
        throw ParseError(1, 1, e.what(), SqlLexer::getLine(sql_, 1));
    }
}

const Token& SqlParser::peek() const {
    if (pos_ >= tokens_.size()) {
        return tokens_.back(); // EOF token
    }
    return tokens_[pos_];
}

const Token& SqlParser::peekAhead(size_t k) const {
    if (pos_ + k >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[pos_ + k];
}

const Token& SqlParser::advance() {
    if (pos_ < tokens_.size()) {
        return tokens_[pos_++];
    }
    return tokens_.back();
}

bool SqlParser::check(TokenKind kind, const std::string& text_lower) const {
    const Token& tok = peek();
    if (tok.kind != kind) return false;
    if (!text_lower.empty() && tok.text_lower != text_lower) return false;
    return true;
}

bool SqlParser::match(TokenKind kind, const std::string& text_lower) {
    if (check(kind, text_lower)) {
        advance();
        return true;
    }
    return false;
}

const Token& SqlParser::expect(TokenKind kind, const std::string& text_lower, const std::string& err_msg) {
    if (check(kind, text_lower)) {
        return advance();
    }
    std::string msg = err_msg;
    if (msg.empty()) {
        msg = "expected '" + (text_lower.empty() ? "token" : text_lower) + "', found '" + peek().text + "'";
    }
    throw makeError(peek(), msg);
}

ParseError SqlParser::makeError(const Token& tok, const std::string& msg) const {
    std::string line_text = SqlLexer::getLine(sql_, tok.line);
    return ParseError(tok.line, tok.col, msg, line_text);
}

SelectStmt SqlParser::parseSelect() {
    if (peek().kind == TokenKind::END_OF_FILE) {
        throw makeError(peek(), "empty input; expected SELECT");
    }

    expect(TokenKind::KEYWORD, "select", "expected SELECT statement");

    SelectStmt stmt;
    if (match(TokenKind::KEYWORD, "distinct")) {
        stmt.distinct = true;
    }

    // Select items
    if (check(TokenKind::KEYWORD, "from")) {
        throw makeError(peek(), "expected select item, found 'FROM'");
    }

    while (true) {
        stmt.items.push_back(parseSelectItem());
        if (match(TokenKind::OPERATOR, ",")) {
            if (check(TokenKind::KEYWORD, "from") || check(TokenKind::END_OF_FILE)) {
                throw makeError(peek(), "trailing comma in select list");
            }
            continue;
        }
        break;
    }

    expect(TokenKind::KEYWORD, "from", "expected FROM clause");
    stmt.from_table = parseTableRef();

    // Optional JOIN clauses
    while (check(TokenKind::KEYWORD, "join") ||
           check(TokenKind::KEYWORD, "inner") ||
           check(TokenKind::KEYWORD, "left") ||
           check(TokenKind::KEYWORD, "right") ||
           check(TokenKind::KEYWORD, "full") ||
           check(TokenKind::KEYWORD, "natural")) {
        stmt.joins.push_back(parseJoinClause());
    }

    // Optional WHERE
    if (match(TokenKind::KEYWORD, "where")) {
        stmt.where_clause = parseExpression();
    }

    // Optional GROUP BY
    if (match(TokenKind::KEYWORD, "group")) {
        expect(TokenKind::KEYWORD, "by", "expected BY after GROUP");
        while (true) {
            stmt.group_by.push_back(parseExpression());
            if (match(TokenKind::OPERATOR, ",")) {
                if (check(TokenKind::KEYWORD, "having") ||
                    check(TokenKind::KEYWORD, "order") ||
                    check(TokenKind::KEYWORD, "limit") ||
                    check(TokenKind::OPERATOR, ";") ||
                    check(TokenKind::END_OF_FILE)) {
                    throw makeError(peek(), "trailing comma in GROUP BY list");
                }
                continue;
            }
            break;
        }
    }

    // Optional HAVING
    if (match(TokenKind::KEYWORD, "having")) {
        stmt.having = parseExpression();
    }

    // Optional ORDER BY
    if (match(TokenKind::KEYWORD, "order")) {
        expect(TokenKind::KEYWORD, "by", "expected BY after ORDER");
        while (true) {
            stmt.order_by.push_back(parseOrderByItem());
            if (match(TokenKind::OPERATOR, ",")) {
                if (check(TokenKind::KEYWORD, "limit") ||
                    check(TokenKind::OPERATOR, ";") ||
                    check(TokenKind::END_OF_FILE)) {
                    throw makeError(peek(), "trailing comma in ORDER BY list");
                }
                continue;
            }
            break;
        }
    }

    // Optional LIMIT [OFFSET]
    if (match(TokenKind::KEYWORD, "limit")) {
        if (check(TokenKind::OPERATOR, "-")) {
            throw makeError(peek(), "LIMIT must be a non-negative integer");
        }
        if (!check(TokenKind::LITERAL_INT)) {
            throw makeError(peek(), "LIMIT requires an integer literal");
        }
        stmt.limit = advance().int_val;

        if (match(TokenKind::KEYWORD, "offset")) {
            if (check(TokenKind::OPERATOR, "-")) {
                throw makeError(peek(), "OFFSET must be a non-negative integer");
            }
            if (!check(TokenKind::LITERAL_INT)) {
                throw makeError(peek(), "OFFSET requires an integer literal");
            }
            stmt.offset = advance().int_val;
        }
    }

    // Optional trailing semicolon
    match(TokenKind::OPERATOR, ";");

    // Enforce strictly one statement: no extra tokens allowed after statement
    if (peek().kind != TokenKind::END_OF_FILE) {
        throw makeError(peek(), "unexpected token after statement: '" + peek().text + "'");
    }

    return stmt;
}

SelectItem SqlParser::parseSelectItem() {
    SelectItem item;

    // Check for * or table.*
    if (check(TokenKind::OPERATOR, "*")) {
        advance();
        auto star_expr = std::make_unique<Expr>();
        star_expr->kind = ExprKind::STAR;
        item.expr = std::move(star_expr);
    } else if (check(TokenKind::IDENTIFIER) && peekAhead(1).kind == TokenKind::OPERATOR && peekAhead(1).text == "." &&
               peekAhead(2).kind == TokenKind::OPERATOR && peekAhead(2).text == "*") {
        std::string table = advance().text; // table name
        advance();                          // consume '.'
        advance();                          // consume '*'
        auto star_expr = std::make_unique<Expr>();
        star_expr->kind = ExprKind::STAR;
        star_expr->table_name = table;
        item.expr = std::move(star_expr);
    } else {
        item.expr = parseExpression();
    }

    // Optional alias: AS <alias> or implicit <alias>
    if (match(TokenKind::KEYWORD, "as")) {
        const Token& alias_tok = expect(TokenKind::IDENTIFIER, "", "expected alias after AS");
        item.alias = alias_tok.text;
    } else if (check(TokenKind::IDENTIFIER)) {
        // Must not be a clause keyword like FROM, WHERE, etc.
        const std::string& next_lower = peek().text_lower;
        if (next_lower != "from" && next_lower != "where" && next_lower != "group" &&
            next_lower != "having" && next_lower != "order" && next_lower != "limit" &&
            next_lower != "join" && next_lower != "inner" && next_lower != "left" &&
            next_lower != "right" && next_lower != "full" && next_lower != "natural" &&
            next_lower != "using") {
            item.alias = advance().text;
        }
    }

    return item;
}

TableRef SqlParser::parseTableRef() {
    TableRef tbl;
    const Token& name_tok = expect(TokenKind::IDENTIFIER, "", "expected table name");
    tbl.name = name_tok.text;

    if (match(TokenKind::KEYWORD, "as")) {
        const Token& alias_tok = expect(TokenKind::IDENTIFIER, "", "expected alias after AS");
        tbl.alias = alias_tok.text;
    } else if (check(TokenKind::IDENTIFIER)) {
        const std::string& next_lower = peek().text_lower;
        if (next_lower != "join" && next_lower != "inner" && next_lower != "left" &&
            next_lower != "right" && next_lower != "full" && next_lower != "where" &&
            next_lower != "group" && next_lower != "having" && next_lower != "order" &&
            next_lower != "limit" && next_lower != "on" && next_lower != "natural" &&
            next_lower != "using") {
            tbl.alias = advance().text;
        }
    }

    return tbl;
}

JoinClause SqlParser::parseJoinClause() {
    JoinClause j;
    j.type = JoinType::INNER;

    if (match(TokenKind::KEYWORD, "natural")) {
        j.is_natural = true;
        if (match(TokenKind::KEYWORD, "inner")) {
            j.type = JoinType::INNER;
            expect(TokenKind::KEYWORD, "join", "expected JOIN after NATURAL INNER");
        } else if (match(TokenKind::KEYWORD, "left")) {
            j.type = JoinType::LEFT;
            match(TokenKind::KEYWORD, "outer");
            expect(TokenKind::KEYWORD, "join", "expected JOIN after NATURAL LEFT [OUTER]");
        } else if (match(TokenKind::KEYWORD, "right")) {
            j.type = JoinType::RIGHT;
            match(TokenKind::KEYWORD, "outer");
            expect(TokenKind::KEYWORD, "join", "expected JOIN after NATURAL RIGHT [OUTER]");
        } else if (match(TokenKind::KEYWORD, "full")) {
            j.type = JoinType::FULL;
            match(TokenKind::KEYWORD, "outer");
            expect(TokenKind::KEYWORD, "join", "expected JOIN after NATURAL FULL [OUTER]");
        } else {
            expect(TokenKind::KEYWORD, "join", "expected JOIN after NATURAL");
        }
        j.table = parseTableRef();
        return j;
    }

    if (match(TokenKind::KEYWORD, "inner")) {
        j.type = JoinType::INNER;
        expect(TokenKind::KEYWORD, "join", "expected JOIN after INNER");
    } else if (match(TokenKind::KEYWORD, "left")) {
        j.type = JoinType::LEFT;
        match(TokenKind::KEYWORD, "outer"); // optional OUTER
        expect(TokenKind::KEYWORD, "join", "expected JOIN after LEFT [OUTER]");
    } else if (match(TokenKind::KEYWORD, "right")) {
        j.type = JoinType::RIGHT;
        match(TokenKind::KEYWORD, "outer"); // optional OUTER
        expect(TokenKind::KEYWORD, "join", "expected JOIN after RIGHT [OUTER]");
    } else if (match(TokenKind::KEYWORD, "full")) {
        j.type = JoinType::FULL;
        match(TokenKind::KEYWORD, "outer"); // optional OUTER
        expect(TokenKind::KEYWORD, "join", "expected JOIN after FULL [OUTER]");
    } else if (match(TokenKind::KEYWORD, "join")) {
        j.type = JoinType::INNER;
    } else {
        throw makeError(peek(), "expected JOIN clause");
    }

    j.table = parseTableRef();
    if (match(TokenKind::KEYWORD, "using")) {
        j.is_using = true;
        expect(TokenKind::OPERATOR, "(", "expected '(' after USING");
        while (true) {
            expect(TokenKind::IDENTIFIER, "", "expected column name in USING");
            if (match(TokenKind::OPERATOR, ",")) continue;
            break;
        }
        expect(TokenKind::OPERATOR, ")", "expected ')' after USING column list");
        return j;
    }
    expect(TokenKind::KEYWORD, "on", "expected ON after JOIN table");
    j.on_expr = parseExpression();

    return j;
}

OrderByItem SqlParser::parseOrderByItem() {
    OrderByItem item;
    item.expr = parseExpression();
    if (match(TokenKind::KEYWORD, "desc")) {
        item.descending = true;
    } else {
        match(TokenKind::KEYWORD, "asc");
        item.descending = false;
    }
    return item;
}

// ----------------------------------------------------------------------------
// Expression Hierarchy with Depth Limit & Operator Precedence
// ----------------------------------------------------------------------------

std::unique_ptr<Expr> SqlParser::parseExpression() {
    DepthGuard guard(depth_, this, peek());
    return parseOr();
}

std::unique_ptr<Expr> SqlParser::parseOr() {
    auto left = parseAnd();

    while (match(TokenKind::KEYWORD, "or")) {
        auto right = parseAnd();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BINARY_OP;
        node->op = "or";
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> SqlParser::parseAnd() {
    auto left = parseNot();

    while (match(TokenKind::KEYWORD, "and")) {
        auto right = parseNot();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BINARY_OP;
        node->op = "and";
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> SqlParser::parseNot() {
    if (match(TokenKind::KEYWORD, "not")) {
        auto child = parseNot();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::UNARY_OP;
        node->op = "not";
        node->child = std::move(child);
        return node;
    }
    return parseComparison();
}

std::unique_ptr<Expr> SqlParser::parseComparison() {
    auto left = parseAdditive();

    // Check IS [NOT] NULL
    if (match(TokenKind::KEYWORD, "is")) {
        bool is_not = match(TokenKind::KEYWORD, "not");
        expect(TokenKind::KEYWORD, "null", "expected NULL after IS [NOT]");
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::IS_NULL;
        node->child = std::move(left);
        node->is_not = is_not;
        return node;
    }

    // Check [NOT] LIKE
    bool is_like = false;
    bool not_like = false;
    if (check(TokenKind::KEYWORD, "not") && peekAhead(1).kind == TokenKind::KEYWORD && peekAhead(1).text_lower == "like") {
        advance(); // consume NOT
        advance(); // consume LIKE
        is_like = true;
        not_like = true;
    } else if (match(TokenKind::KEYWORD, "like")) {
        is_like = true;
        not_like = false;
    }

    if (is_like) {
        auto pattern = parseAdditive();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::LIKE;
        node->is_not = not_like;
        node->like_expr = std::move(left);
        node->like_pattern = std::move(pattern);
        return node;
    }

    // Check [NOT] BETWEEN
    bool is_between = false;
    bool not_between = false;
    if (check(TokenKind::KEYWORD, "not") && peekAhead(1).kind == TokenKind::KEYWORD && peekAhead(1).text_lower == "between") {
        advance(); // consume NOT
        advance(); // consume BETWEEN
        is_between = true;
        not_between = true;
    } else if (match(TokenKind::KEYWORD, "between")) {
        is_between = true;
        not_between = false;
    }

    if (is_between) {
        // Change 4: Parse BETWEEN bounds with parseAdditive() so AND is not swallowed by boolean AND!
        auto low = parseAdditive();
        expect(TokenKind::KEYWORD, "and", "expected AND in BETWEEN expression");
        auto high = parseAdditive();

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BETWEEN;
        node->is_not = not_between;
        node->between_expr = std::move(left);
        node->between_low = std::move(low);
        node->between_high = std::move(high);
        return node;
    }

    // Check [NOT] IN (list)
    bool is_in = false;
    bool not_in = false;
    if (check(TokenKind::KEYWORD, "not") && peekAhead(1).kind == TokenKind::KEYWORD && peekAhead(1).text_lower == "in") {
        advance(); // consume NOT
        advance(); // consume IN
        is_in = true;
        not_in = true;
    } else if (match(TokenKind::KEYWORD, "in")) {
        is_in = true;
        not_in = false;
    }

    if (is_in) {
        expect(TokenKind::OPERATOR, "(", "expected '(' after IN");
        std::vector<std::unique_ptr<Expr>> list;
        while (true) {
            list.push_back(parseExpression());
            if (match(TokenKind::OPERATOR, ",")) {
                if (check(TokenKind::OPERATOR, ")")) {
                    throw makeError(peek(), "trailing comma in IN list");
                }
                continue;
            }
            break;
        }
        expect(TokenKind::OPERATOR, ")", "expected ')' after IN list");

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::IN_LIST;
        node->is_not = not_in;
        node->in_expr = std::move(left);
        node->in_list = std::move(list);
        return node;
    }

    // Binary comparison operators: =, !=, <>, <, <=, >, >=
    if (check(TokenKind::OPERATOR, "=") ||
        check(TokenKind::OPERATOR, "!=") ||
        check(TokenKind::OPERATOR, "<>") ||
        check(TokenKind::OPERATOR, "<") ||
        check(TokenKind::OPERATOR, "<=") ||
        check(TokenKind::OPERATOR, ">") ||
        check(TokenKind::OPERATOR, ">=")) {
        std::string op = advance().text;
        auto right = parseAdditive();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BINARY_OP;
        // Normalize <> to != per spec
        node->op = (op == "<>") ? "!=" : op;
        node->left = std::move(left);
        node->right = std::move(right);
        return node;
    }

    return left;
}

std::unique_ptr<Expr> SqlParser::parseAdditive() {
    auto left = parseMultiplicative();

    while (check(TokenKind::OPERATOR, "+") || check(TokenKind::OPERATOR, "-")) {
        std::string op = advance().text;
        auto right = parseMultiplicative();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BINARY_OP;
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> SqlParser::parseMultiplicative() {
    auto left = parseUnary();

    while (check(TokenKind::OPERATOR, "*") || check(TokenKind::OPERATOR, "/")) {
        std::string op = advance().text;
        auto right = parseUnary();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::BINARY_OP;
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> SqlParser::parseUnary() {
    if (match(TokenKind::OPERATOR, "-")) {
        // Unary minus: represented as unary op everywhere per plan
        auto child = parseUnary();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::UNARY_OP;
        node->op = "-";
        node->child = std::move(child);
        return node;
    } else if (match(TokenKind::OPERATOR, "+")) {
        // Unary plus: no-op, just return child
        return parseUnary();
    }
    return parsePrimary();
}

std::unique_ptr<Expr> SqlParser::parsePrimary() {
    DepthGuard guard(depth_, this, peek());

    const Token& tok = peek();

    if (tok.kind == TokenKind::LITERAL_INT) {
        advance();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::LITERAL_INT;
        node->int_val = tok.int_val;
        return node;
    }

    if (tok.kind == TokenKind::LITERAL_FLOAT) {
        advance();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::LITERAL_FLOAT;
        node->float_val = tok.float_val;
        node->str_val = tok.text;
        return node;
    }

    if (tok.kind == TokenKind::LITERAL_STRING) {
        advance();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::LITERAL_STRING;
        node->str_val = tok.text;
        return node;
    }

    if (tok.kind == TokenKind::KEYWORD && tok.text_lower == "null") {
        advance();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::LITERAL_NULL;
        return node;
    }

    if (match(TokenKind::OPERATOR, "(")) {
        auto expr = parseExpression();
        expect(TokenKind::OPERATOR, ")", "unbalanced parenthesis; expected ')'");
        return expr;
    }

    if (match(TokenKind::OPERATOR, "*")) {
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::STAR;
        return node;
    }

    // Identifier: could be a function call, qualified column (t.col or t.*), or unqualified column
    if (tok.kind == TokenKind::IDENTIFIER) {
        std::string ident = advance().text;

        // Check function call: ident followed by '('
        if (check(TokenKind::OPERATOR, "(")) {
            advance(); // consume '('
            auto func_node = std::make_unique<Expr>();
            func_node->kind = ExprKind::FUNCTION_CALL;
            // Function name is lowercased for canonical display
            std::string lower_func;
            for (char c : ident) lower_func.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
            func_node->func_name = lower_func;

            if (match(TokenKind::OPERATOR, "*")) {
                func_node->func_star = true;
            } else if (match(TokenKind::KEYWORD, "distinct")) {
                func_node->func_distinct = true;
                func_node->func_args.push_back(parseExpression());
            } else if (!check(TokenKind::OPERATOR, ")")) {
                while (true) {
                    func_node->func_args.push_back(parseExpression());
                    if (match(TokenKind::OPERATOR, ",")) {
                        continue;
                    }
                    break;
                }
            }
            expect(TokenKind::OPERATOR, ")", "expected ')' after function arguments");
            return func_node;
        }

        // Check qualified identifier: ident.col or ident.*
        if (match(TokenKind::OPERATOR, ".")) {
            if (match(TokenKind::OPERATOR, "*")) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::STAR;
                node->table_name = ident;
                return node;
            }
            const Token& col_tok = expect(TokenKind::IDENTIFIER, "", "expected column name or * after '.'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::COLUMN_REF;
            node->table_name = ident;
            node->col_name = col_tok.text;
            return node;
        }

        // Unqualified column reference
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::COLUMN_REF;
        node->table_name = ""; // Unqualified per Change 2
        node->col_name = ident;
        return node;
    }

    throw makeError(tok, "unexpected token in expression: '" + tok.text + "'");
}

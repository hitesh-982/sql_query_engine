#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include "sql_lexer.h"
#include "sql_ast.h"

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

struct ParseError : public std::exception {
    size_t line;
    size_t col;
    std::string message;
    std::string sql_line;
    std::string formatted;

    ParseError(size_t l, size_t c, const std::string& msg, const std::string& line_text);
    const char* what() const noexcept override {
        return formatted.c_str();
    }
};

class SqlParser {
public:
    explicit SqlParser(std::string_view sql);

    // Parses a single SELECT statement. Throws ParseError on failure.
    SelectStmt parseSelect();

private:
    std::string_view sql_;
    std::vector<Token> tokens_;
    size_t pos_;
    int depth_; // Nesting depth limit (max 200)

    const Token& peek() const;
    const Token& peekAhead(size_t k) const;
    const Token& advance();
    bool check(TokenKind kind, const std::string& text_lower = "") const;
    bool match(TokenKind kind, const std::string& text_lower = "");
    const Token& expect(TokenKind kind, const std::string& text_lower = "", const std::string& err_msg = "");

    ParseError makeError(const Token& tok, const std::string& msg) const;

    // Statement clauses
    SelectItem parseSelectItem();
    TableRef parseTableRef();
    JoinClause parseJoinClause();
    OrderByItem parseOrderByItem();

    // Expression hierarchy (ordered by precedence)
    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();
    std::unique_ptr<Expr> parseNot();
    std::unique_ptr<Expr> parseComparison();
    std::unique_ptr<Expr> parseAdditive();
    std::unique_ptr<Expr> parseMultiplicative();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePrimary();

    // Recursion depth guard helper
    struct DepthGuard {
        int& d;
        const SqlParser* parser;
        const Token& tok;
        DepthGuard(int& depth, const SqlParser* p, const Token& t) : d(depth), parser(p), tok(t) {
            if (++d > 200) {
                throw parser->makeError(tok, "exceeded maximum nesting depth of 200");
            }
        }
        ~DepthGuard() {
            --d;
        }
    };
};

#endif // SQL_PARSER_H

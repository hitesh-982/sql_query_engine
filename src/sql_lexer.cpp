#include "sql_lexer.h"

#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <cerrno>
#include <cstdlib>

// Case-insensitive keywords.
// NOTE: COUNT, SUM, AVG, MIN, MAX are deliberately NOT reserved keywords;
// they are ordinary identifiers so queries like "SELECT count FROM t" parse as columns.
static const std::unordered_set<std::string> RESERVED_KEYWORDS = {
    "select", "distinct", "from", "as", "join", "inner", "left", "right",
    "full", "outer", "on", "where", "group", "by", "having", "order",
    "asc", "desc", "limit", "offset", "and", "or", "not", "is", "null",
    "in", "between", "like", "true", "false", "natural", "using"
};

SqlLexer::SqlLexer(std::string_view sql)
    : sql_(sql), pos_(0), line_(1), col_(1) {}

char SqlLexer::peek() const {
    if (pos_ >= sql_.size()) return '\0';
    return sql_[pos_];
}

char SqlLexer::peekNext() const {
    if (pos_ + 1 >= sql_.size()) return '\0';
    return sql_[pos_ + 1];
}

char SqlLexer::advance() {
    if (pos_ >= sql_.size()) return '\0';
    char c = sql_[pos_++];
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

bool SqlLexer::match(char expected) {
    if (peek() == expected) {
        advance();
        return true;
    }
    return false;
}

void SqlLexer::skipWhitespaceAndComments() {
    while (pos_ < sql_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && peekNext() == '-') {
            // Line comment -- ... up to newline
            advance(); // Consume '-'
            advance(); // Consume '-'
            while (pos_ < sql_.size() && peek() != '\n') {
                advance();
            }
        } else if (c == '/' && peekNext() == '*') {
            // Block comment /* ... */
            size_t start_line = line_;
            size_t start_col = col_;
            advance(); // Consume '/'
            advance(); // Consume '*'
            bool closed = false;
            while (pos_ < sql_.size()) {
                if (peek() == '*' && peekNext() == '/') {
                    advance(); // Consume '*'
                    advance(); // Consume '/'
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                std::ostringstream oss;
                oss << "unterminated block comment at line " << start_line << ", column " << start_col;
                throw std::runtime_error(oss.str());
            }
        } else {
            break;
        }
    }
}

Token SqlLexer::scanNumber() {
    size_t start_line = line_;
    size_t start_col = col_;
    size_t start_pos = pos_;

    bool is_float = false;

    // Digits before dot
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    // Optional fractional part
    if (peek() == '.') {
        is_float = true;
        advance(); // Consume '.'
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    // Optional exponent [eE][+-]?[0-9]+
    if (peek() == 'e' || peek() == 'E') {
        is_float = true;
        advance(); // Consume 'e' / 'E'
        if (peek() == '+' || peek() == '-') {
            advance();
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    std::string text(sql_.substr(start_pos, pos_ - start_pos));
    Token tok;
    tok.line = start_line;
    tok.col = start_col;
    tok.text = text;
    tok.text_lower = text;

    if (!is_float) {
        // Try parsing as 64-bit integer
        long long val = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec == std::errc{} && ptr == text.data() + text.size()) {
            tok.kind = TokenKind::LITERAL_INT;
            tok.int_val = val;
            return tok;
        }
        // SQLite rule: integer exceeding 64-bit range converts to a floating-point number
        tok.kind = TokenKind::LITERAL_FLOAT;
        tok.float_val = std::strtod(text.c_str(), nullptr);
        return tok;
    } else {
        tok.kind = TokenKind::LITERAL_FLOAT;
        tok.float_val = std::strtod(text.c_str(), nullptr);
        return tok;
    }
}

Token SqlLexer::scanIdentifierOrKeyword() {
    size_t start_line = line_;
    size_t start_col = col_;
    size_t start_pos = pos_;

    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        advance();
    }

    std::string text(sql_.substr(start_pos, pos_ - start_pos));
    std::string lower_text;
    lower_text.reserve(text.size());
    for (char c : text) {
        lower_text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    Token tok;
    tok.line = start_line;
    tok.col = start_col;
    tok.text = text;
    tok.text_lower = lower_text;

    if (RESERVED_KEYWORDS.find(lower_text) != RESERVED_KEYWORDS.end()) {
        tok.kind = TokenKind::KEYWORD;
    } else {
        tok.kind = TokenKind::IDENTIFIER;
    }
    return tok;
}

Token SqlLexer::scanQuotedIdentifier() {
    size_t start_line = line_;
    size_t start_col = col_;
    advance(); // Consume opening '"'

    std::string val;
    bool closed = false;

    while (pos_ < sql_.size()) {
        char c = advance();
        if (c == '"') {
            if (peek() == '"') {
                // Escaped double quote inside quoted identifier
                val.push_back('"');
                advance();
            } else {
                closed = true;
                break;
            }
        } else {
            val.push_back(c);
        }
    }

    if (!closed) {
        std::ostringstream oss;
        oss << "unterminated quoted identifier at line " << start_line << ", column " << start_col;
        throw std::runtime_error(oss.str());
    }

    Token tok;
    tok.kind = TokenKind::IDENTIFIER;
    tok.line = start_line;
    tok.col = start_col;
    tok.text = val;
    tok.text_lower = val; // Case preserved for quoted identifiers
    return tok;
}

Token SqlLexer::scanString() {
    size_t start_line = line_;
    size_t start_col = col_;
    advance(); // Consume opening '\''

    std::string val;
    bool closed = false;

    while (pos_ < sql_.size()) {
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') {
                // '' represents an escaped single quote in SQL
                val.push_back('\'');
                advance();
            } else {
                closed = true;
                break;
            }
        } else {
            val.push_back(c);
        }
    }

    if (!closed) {
        std::ostringstream oss;
        oss << "unterminated string literal at line " << start_line << ", column " << start_col;
        throw std::runtime_error(oss.str());
    }

    Token tok;
    tok.kind = TokenKind::LITERAL_STRING;
    tok.line = start_line;
    tok.col = start_col;
    tok.text = val;
    tok.text_lower = val;
    return tok;
}

std::vector<Token> SqlLexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();
        if (pos_ >= sql_.size()) {
            tokens.push_back({TokenKind::END_OF_FILE, "", "", line_, col_, 0, 0.0});
            break;
        }

        char c = peek();
        size_t cur_line = line_;
        size_t cur_col = col_;

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(scanIdentifierOrKeyword());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(scanNumber());
        } else if (c == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
            // Numbers with leading dot, e.g. .5
            tokens.push_back(scanNumber());
        } else if (c == '"') {
            tokens.push_back(scanQuotedIdentifier());
        } else if (c == '\'') {
            tokens.push_back(scanString());
        } else {
            // Operator / Punctuation
            advance();
            std::string op(1, c);

            if (c == '!' && peek() == '=') {
                advance();
                op = "!=";
            } else if (c == '<' && peek() == '>') {
                advance();
                op = "<>";
            } else if (c == '<' && peek() == '=') {
                advance();
                op = "<=";
            } else if (c == '>' && peek() == '=') {
                advance();
                op = ">=";
            }

            Token tok;
            tok.kind = TokenKind::OPERATOR;
            tok.line = cur_line;
            tok.col = cur_col;
            tok.text = op;
            tok.text_lower = op;
            tokens.push_back(tok);
        }
    }

    return tokens;
}

std::string SqlLexer::getLine(std::string_view sql, size_t target_line) {
    size_t current_line = 1;
    size_t line_start = 0;

    for (size_t i = 0; i <= sql.size(); ++i) {
        if (i == sql.size() || sql[i] == '\n') {
            if (current_line == target_line) {
                size_t len = i - line_start;
                if (len > 0 && sql[line_start + len - 1] == '\r') {
                    len--;
                }
                return std::string(sql.substr(line_start, len));
            }
            current_line++;
            line_start = i + 1;
        }
    }
    return "";
}

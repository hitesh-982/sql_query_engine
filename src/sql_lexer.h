#ifndef SQL_LEXER_H
#define SQL_LEXER_H

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

enum class TokenKind {
    KEYWORD,
    IDENTIFIER,
    LITERAL_INT,
    LITERAL_FLOAT,
    LITERAL_STRING,
    OPERATOR,
    END_OF_FILE
};

struct Token {
    TokenKind kind;
    std::string text;       // Original spelling
    std::string text_lower; // Lowercased for case-insensitive matching
    size_t line;            // 1-based line number
    size_t col;             // 1-based column number
    long long int_val = 0;  // Valid if kind == LITERAL_INT
    double float_val = 0.0; // Valid if kind == LITERAL_FLOAT
};

class SqlLexer {
public:
    explicit SqlLexer(std::string_view sql);

    // Tokenizes entire input. Throws std::string on lex error with position.
    std::vector<Token> tokenize();

    // Helper to get line content for error printing
    static std::string getLine(std::string_view sql, size_t target_line);

private:
    std::string_view sql_;
    size_t pos_;
    size_t line_;
    size_t col_;

    char peek() const;
    char peekNext() const;
    char advance();
    bool match(char expected);
    void skipWhitespaceAndComments();

    Token scanNumber();
    Token scanIdentifierOrKeyword();
    Token scanQuotedIdentifier();
    Token scanString();
};

#endif // SQL_LEXER_H

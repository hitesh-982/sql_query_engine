#include "sql_lexer.h"

#include <cassert>
#include <iostream>
#include <cmath>

static void testCase1() {
    // "1--1" -> 1 followed by comment "--1" -> single token: integer 1
    SqlLexer lex("1--1");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 2); // INT 1 and EOF
    assert(tokens[0].kind == TokenKind::LITERAL_INT);
    assert(tokens[0].int_val == 1);
    assert(tokens[1].kind == TokenKind::END_OF_FILE);
    std::cout << "[OK] 1--1 test passed\n";
}

static void testCase2() {
    // "1 - -1" -> 1, -, -, 1
    SqlLexer lex("1 - -1");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 5); // 1, -, -, 1, EOF
    assert(tokens[0].kind == TokenKind::LITERAL_INT && tokens[0].int_val == 1);
    assert(tokens[1].kind == TokenKind::OPERATOR && tokens[1].text == "-");
    assert(tokens[2].kind == TokenKind::OPERATOR && tokens[2].text == "-");
    assert(tokens[3].kind == TokenKind::LITERAL_INT && tokens[3].int_val == 1);
    std::cout << "[OK] 1 - -1 test passed\n";
}

static void testCase3() {
    // ".5" -> float 0.5
    SqlLexer lex(".5");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 2);
    assert(tokens[0].kind == TokenKind::LITERAL_FLOAT);
    assert(std::fabs(tokens[0].float_val - 0.5) < 1e-9);
    std::cout << "[OK] .5 test passed\n";
}

static void testCase4() {
    // "5." -> float 5.0
    SqlLexer lex("5.");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 2);
    assert(tokens[0].kind == TokenKind::LITERAL_FLOAT);
    assert(std::fabs(tokens[0].float_val - 5.0) < 1e-9);
    std::cout << "[OK] 5. test passed\n";
}

static void testCase5() {
    // "1e3" -> float 1000.0
    SqlLexer lex("1e3");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 2);
    assert(tokens[0].kind == TokenKind::LITERAL_FLOAT);
    assert(std::fabs(tokens[0].float_val - 1000.0) < 1e-9);
    std::cout << "[OK] 1e3 test passed\n";
}

static void testCase6() {
    // "99999999999999999999" -> out-of-range integer converted to float (SQLite behavior)
    SqlLexer lex("99999999999999999999");
    auto tokens = lex.tokenize();
    assert(tokens.size() == 2);
    assert(tokens[0].kind == TokenKind::LITERAL_FLOAT);
    assert(tokens[0].float_val > 9e19);
    std::cout << "[OK] 99999999999999999999 test passed\n";
}

static void testCase7() {
    // Unterminated string
    bool caught = false;
    try {
        SqlLexer lex("'unterminated");
        lex.tokenize();
    } catch (const std::exception& e) {
        caught = true;
    }
    assert(caught);
    std::cout << "[OK] unterminated string test passed\n";
}

static void testCase8() {
    // Unterminated block comment
    bool caught = false;
    try {
        SqlLexer lex("/* unterminated comment");
        lex.tokenize();
    } catch (const std::exception& e) {
        caught = true;
    }
    assert(caught);
    std::cout << "[OK] unterminated block comment test passed\n";
}

static void testCase9() {
    // COUNT, SUM, AVG, MIN, MAX as identifiers
    SqlLexer lex("SELECT count, min FROM t");
    auto tokens = lex.tokenize();
    assert(tokens[0].kind == TokenKind::KEYWORD); // SELECT
    assert(tokens[1].kind == TokenKind::IDENTIFIER && tokens[1].text_lower == "count");
    assert(tokens[2].kind == TokenKind::OPERATOR && tokens[2].text == ",");
    assert(tokens[3].kind == TokenKind::IDENTIFIER && tokens[3].text_lower == "min");
    assert(tokens[4].kind == TokenKind::KEYWORD && tokens[4].text_lower == "from");
    assert(tokens[5].kind == TokenKind::IDENTIFIER && tokens[5].text_lower == "t");
    std::cout << "[OK] count/min as identifiers test passed\n";
}

int main() {
    std::cout << "--- Running Lexer Unit Tests ---\n";
    testCase1();
    testCase2();
    testCase3();
    testCase4();
    testCase5();
    testCase6();
    testCase7();
    testCase8();
    testCase9();
    std::cout << "All lexer unit tests PASSED!\n";
    return 0;
}

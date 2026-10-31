CXX ?= $(shell command -v /usr/bin/g++ 2>/dev/null || command -v g++)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra

SRCS_PARSER = src/sql_lexer.cpp src/sql_ast.cpp src/sql_parser.cpp
OBJS_PARSER = $(SRCS_PARSER:.cpp=.o)

SRCS_CORE = src/csv_scan.cpp src/value.cpp src/expr_eval.cpp src/ops_basic.cpp src/planner.cpp $(SRCS_PARSER)
OBJS_CORE = $(SRCS_CORE:.cpp=.o)

SRCS_ENGINE = src/main.cpp $(SRCS_CORE)
OBJS_ENGINE = src/main.o $(OBJS_CORE)

SRCS_SQLPARSE = src/sqlparse_main.cpp $(SRCS_PARSER)
OBJS_SQLPARSE = src/sqlparse_main.o $(OBJS_PARSER)

SRCS_TEST_LEXER = src/test_lexer.cpp src/sql_lexer.cpp
OBJS_TEST_LEXER = src/test_lexer.o src/sql_lexer.o

SRCS_TEST_EVAL = tests/test_eval.cpp $(SRCS_CORE)
OBJS_TEST_EVAL = tests/test_eval.o $(OBJS_CORE)

all: engine sqlparse test_lexer test_eval

engine: $(OBJS_ENGINE)
	$(CXX) $(CXXFLAGS) -o $@ $^

sqlparse: $(OBJS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_lexer: $(OBJS_TEST_LEXER)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_eval: $(OBJS_TEST_EVAL)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Target with AddressSanitizer and UndefinedBehaviorSanitizer for fuzz testing
sqlparse_sanitized: $(SRCS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o tests/*.o engine sqlparse test_lexer test_eval sqlparse_sanitized

.PHONY: all clean

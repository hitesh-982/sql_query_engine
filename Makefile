CXX ?= $(shell command -v /usr/bin/g++ 2>/dev/null || command -v g++)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra

SRCS_COMMON = src/sql_lexer.cpp src/sql_ast.cpp src/sql_parser.cpp
OBJS_COMMON = $(SRCS_COMMON:.cpp=.o)

SRCS_ENGINE = src/main.cpp src/csv_scan.cpp $(SRCS_COMMON)
OBJS_ENGINE = src/main.o src/csv_scan.o $(OBJS_COMMON)

SRCS_SQLPARSE = src/sqlparse_main.cpp $(SRCS_COMMON)
OBJS_SQLPARSE = src/sqlparse_main.o $(OBJS_COMMON)

SRCS_TEST_LEXER = src/test_lexer.cpp src/sql_lexer.cpp
OBJS_TEST_LEXER = src/test_lexer.o src/sql_lexer.o

all: engine sqlparse test_lexer

engine: $(OBJS_ENGINE)
	$(CXX) $(CXXFLAGS) -o $@ $^

sqlparse: $(OBJS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_lexer: $(OBJS_TEST_LEXER)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Target with AddressSanitizer and UndefinedBehaviorSanitizer for fuzz testing
sqlparse_sanitized: $(SRCS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o engine sqlparse test_lexer sqlparse_sanitized

.PHONY: all clean

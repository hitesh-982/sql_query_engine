CXX ?= $(shell command -v /usr/bin/g++ 2>/dev/null || command -v g++)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra

SRCS_PARSER = src/sql_lexer.cpp src/sql_ast.cpp src/sql_parser.cpp
OBJS_PARSER = $(SRCS_PARSER:.cpp=.o)

SRCS_CORE = src/csv_scan.cpp src/value.cpp src/expr_eval.cpp src/ops_basic.cpp src/ops_agg.cpp src/group_key.cpp src/ops_sort.cpp src/ops_distinct.cpp src/ops_join.cpp src/planner.cpp $(SRCS_PARSER)
OBJS_CORE = $(SRCS_CORE:.cpp=.o)

SRCS_ENGINE = src/main.cpp $(SRCS_CORE)
OBJS_ENGINE = src/main.o $(OBJS_CORE)

SRCS_SQLPARSE = src/sqlparse_main.cpp $(SRCS_PARSER)
OBJS_SQLPARSE = src/sqlparse_main.o $(OBJS_PARSER)

SRCS_TEST_LEXER = src/test_lexer.cpp src/sql_lexer.cpp
OBJS_TEST_LEXER = src/test_lexer.o src/sql_lexer.o

SRCS_TEST_EVAL = tests/test_eval.cpp $(SRCS_CORE)
OBJS_TEST_EVAL = tests/test_eval.o $(OBJS_CORE)

SRCS_TEST_AGG = tests/test_agg.cpp $(SRCS_CORE)
OBJS_TEST_AGG = tests/test_agg.o $(OBJS_CORE)

SRCS_TEST_SORT = tests/test_sort.cpp $(SRCS_CORE)
OBJS_TEST_SORT = tests/test_sort.o $(OBJS_CORE)

SRCS_TEST_JOIN = tests/test_join.cpp $(SRCS_CORE)
OBJS_TEST_JOIN = tests/test_join.o $(OBJS_CORE)

all: engine sqlparse test_lexer test_eval test_agg test_sort test_join

engine: $(OBJS_ENGINE)
	$(CXX) $(CXXFLAGS) -o $@ $^

sqlparse: $(OBJS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_lexer: $(OBJS_TEST_LEXER)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_eval: $(OBJS_TEST_EVAL)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_agg: $(OBJS_TEST_AGG)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_sort: $(OBJS_TEST_SORT)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_join: $(OBJS_TEST_JOIN)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Target with AddressSanitizer and UndefinedBehaviorSanitizer for fuzz testing
sqlparse_sanitized: $(SRCS_SQLPARSE)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ $^

profile: $(SRCS_ENGINE)
	$(CXX) -O2 -g -fno-omit-frame-pointer -pg -std=c++17 -Wall -Wextra -o engine_profile $^

asan: $(SRCS_ENGINE)
	$(CXX) -O2 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -o engine_asan $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o tests/*.o engine sqlparse test_lexer test_eval test_agg test_sort test_join sqlparse_sanitized engine_profile engine_asan gmon.out

.PHONY: all clean profile asan

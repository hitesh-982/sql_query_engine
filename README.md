# SQL Query Engine in C++17

A lightweight, zero-dependency SQL query engine written in POSIX C++17 for processing multi-GB CSV files with high performance.

---

## Architecture & Phases

### Phase 0: Tooling & Test Framework
- **`tools/gen_data.py`**: High-performance streaming synthetic dataset generator. Supports Zipf-skewed distributions, NULL fields, quoted CSV fields with escaped quotes (`""`), and edge case files (`empty_header_only.csv`, `one_row.csv`, `all_null_column.csv`, `one_giant_group.csv`, `crlf.csv`, `no_trailing_newline.csv`), as well as a second `customers` table for future join testing.
- **`tools/compare.py`**: SQLite `:memory:` test runner. Automatically infers SQLite types matching engine rules, executes queries via engine and SQLite, normalizes outputs, and compares results (including float tolerances of `1e-9` and NULL-first ordering).
- **`tests/queries.json`**: Baseline scan queries and SQL CLI hook queries.
- **`tests/run_tests.sh`**: Master test orchestrator.

### Phase 1: Fast CSV Scanner (`CsvScanner`)
- Read-only memory mapping via `mmap` with aggressive sequential read-ahead (`madvise(MADV_SEQUENTIAL)`).
- **Zero-allocation Fast Path**: Scan unquoted rows using SIMD-accelerated `memchr` and `std::string_view` slices into mmap memory with zero heap allocations per row.
- **RFC 4180 Slow Path**: Quoted fields with embedded commas and `""` quote escapes are unescaped into a persistent `std::deque<std::string>` arena that guarantees pointer stability until `close()`.
- **Automatic Type Inference**: Infers column types (`INT`, `FLOAT`, `STRING`) over the first 10,000 data rows.
- **Number Parsing**: Fast `std::from_chars` for 64-bit integer and IEEE 754 floating-point numbers without locale locks or memory allocation.
- **Benchmark Performance**: Scans 100,000 rows in ~22 milliseconds (>240 MB/s).

### Phase 4: SQL Parser & AST
- **Hand-written Lexer (`src/sql_lexer.h/.cpp`)**:
  - Case-insensitive SQL keywords.
  - `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` are treated as ordinary identifiers (matching SQLite) so queries like `SELECT count FROM t` parse as column references.
  - Unquoted identifiers are normalized to lowercase for lookups while preserving original text; quoted `"identifiers"` preserve exact casing.
  - Numbers: Supports integers, floats with leading/trailing dots (`.5`, `5.`), and scientific notation (`1e3`). Integers exceeding 64-bit range (`> LLONG_MAX`) convert to floating-point numbers (matching SQLite behavior).
  - Skips whitespace, `--` line comments, and `/* ... */` block comments.
- **AST & Serializer (`src/sql_ast.h/.cpp`)**:
  - Plain structs managed via `std::unique_ptr<Expr>`.
  - Canonical S-expression printer (`toSExpr()`) producing canonical expressions:
    - Unqualified column: `(col col_name)`
    - Qualified column: `(col table_name col_name)`
    - `<>` normalized to `!=`
    - Negative literals represented uniformly as unary minus: `(- (int 5))`
    - `IS NULL` -> `(is_null <expr> true)`; `IS NOT NULL` -> `(is_null <expr> false)`
    - `COUNT(*)` -> `(agg count *)`; `COUNT(DISTINCT x)` -> `(agg count (distinct <expr>))`
  - Canonical SQL generator (`toSql()`) for round-trip validation.
- **Recursive-descent Parser (`src/sql_parser.h/.cpp`)**:
  - Operator precedence: `OR` < `AND` < `NOT` < Comparisons/IS/IN/BETWEEN/LIKE < `+ -` < `* /` < unary minus < primary.
  - `BETWEEN` bounds parsed via `parseAdditive()` so the inner `AND` cannot be swallowed by boolean `AND`.
  - Nesting depth limit of 200 to prevent stack overflows on malicious deeply nested inputs.
  - Descriptive `ParseError` exceptions with 1-based line, column, and caret indicators.
  - Single-statement enforcement (tokens after optional trailing `;` are rejected).
  - Negative `LIMIT` is rejected with a parse error (note: SQLite treats negative LIMIT as "no limit").
- **Fuzzing & Sanitization (`tools/fuzz_parser.py`)**:
  - 10,000 random token sequences and query mutations run against `sqlparse_sanitized` built with `-fsanitize=address,undefined`.

---

## Build Instructions

Requirements: POSIX environment (Linux / macOS), C++17 compiler (`g++` or `clang++`), Python 3.

```bash
# Build engine, sqlparse, test_lexer, and sanitized fuzzer binary
make

# Clean build artifacts
make clean
```

---

## CLI Usage

### Engine CLI (`./engine`)
```bash
# Select columns in file order
./engine --file data.csv --select city,qty

# Select all columns
./engine --file data.csv --select '*'

# Inspect inferred schema
./engine --file data.csv --schema

# Benchmark scan throughput
./engine --file data.csv --count

# SQL query mode (Phase 4 engine hook)
./engine --table t=data.csv --sql "SELECT city, qty FROM t LIMIT 10"
```

### SQL Parser CLI (`./sqlparse`)
```bash
# Parse SQL and print canonical AST S-expression
./sqlparse "SELECT city, sum(amount) AS total FROM t GROUP BY city LIMIT 5;"

# Parse from file
./sqlparse -f query.sql

# Output canonical SQL
./sqlparse --to-sql "select A, B from T where X between 1 and 10"

# Verify round-trip idempotency (SQL -> AST -> SQL -> AST)
./sqlparse --roundtrip "SELECT * FROM t;"
```

---

## Testing

```bash
# Run complete test suite (scanner, edge cases, SQLite comparisons, parser tests, fuzzer)
tests/run_tests.sh

# Run lexer unit tests
./test_lexer

# Run parser unit tests (45 positive, 45 round-trip, 27 negative, 100k depth limit)
python3 tools/run_parser_tests.py

# Run full 10,000-query AddressSanitizer/UBSan fuzzer
python3 tools/fuzz_parser.py --count 10000
```

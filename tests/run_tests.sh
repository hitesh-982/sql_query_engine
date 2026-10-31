#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Prefer /usr/bin/python3 or system python to avoid sandbox issues with custom environments
if [ -x "/usr/bin/python3" ]; then
    PYTHON="/usr/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
else
    PYTHON="python"
fi

echo "=== Running SQL Query Engine Test Suite ==="

FAILED=0
DATA_DIR="$REPO_ROOT/tests/data"
EDGE_DIR="$DATA_DIR/edge"
mkdir -p "$EDGE_DIR"

# 1. Build project
if [ -f "Makefile" ]; then
    echo "--- Building project ---"
    if make; then
        echo "[OK] Build succeeded"
    else
        echo "[FAIL] Build failed"
        exit 1
    fi
else
    echo "[INFO] No Makefile present yet."
fi

# 2. Run unit tests
if [ -x "./test_eval" ]; then
    echo "--- Running test_eval unit tests ---"
    if ./test_eval; then
        echo "[OK] test_eval passed"
    else
        echo "[FAIL] test_eval failed"
        FAILED=1
    fi
fi

if [ -x "./test_agg" ]; then
    echo "--- Running test_agg unit tests ---"
    if ./test_agg; then
        echo "[OK] test_agg passed"
    else
        echo "[FAIL] test_agg failed"
        FAILED=1
    fi
fi

if [ -x "./test_lexer" ]; then
    echo "--- Running test_lexer unit tests ---"
    if ./test_lexer; then
        echo "[OK] test_lexer passed"
    else
        echo "[FAIL] test_lexer failed"
        FAILED=1
    fi
fi

# 3. Generate 100k-row test dataset and edge datasets
DATA_100K="$DATA_DIR/data_100k.csv"
if [ ! -f "$DATA_100K" ]; then
    echo "--- Generating 100k dataset and edge files ---"
    "$PYTHON" tools/gen_data.py --rows 100000 --out "$DATA_100K" --seed 42 --edge "$EDGE_DIR"
    echo "[OK] Generated $DATA_100K and edge files in $EDGE_DIR"
else
    echo "[INFO] Dataset $DATA_100K already exists. Generating edge files..."
    "$PYTHON" tools/gen_data.py --rows 10 --out "$DATA_DIR/data_small.csv" --seed 42 --edge "$EDGE_DIR"
fi

# Also generate customers table for join testing
CUSTOMERS_CSV="$DATA_DIR/customers.csv"
if [ ! -f "$CUSTOMERS_CSV" ]; then
    "$PYTHON" tools/gen_data.py --rows 1000 --out "$CUSTOMERS_CSV" --seed 42 --table customers
fi

if [ ! -f "$DATA_DIR/big_values.csv" ]; then
    printf "val\n9223372036854775800\n100\n" > "$DATA_DIR/big_values.csv"
fi
if [ ! -f "$DATA_DIR/text_sum.csv" ]; then
    printf "val\n12abc\n1.5x\nabc\n7\n\" 8 \"\n" > "$DATA_DIR/text_sum.csv"
fi
if [ ! -f "$DATA_DIR/nan_inf.csv" ]; then
    printf "id,val\n1,nan\n2,inf\n3,-inf\n4,Infinity\n5,0x1p3\n6,42\n7,3.14\n" > "$DATA_DIR/nan_inf.csv"
fi

# 4. Run error tests (exit 2 for semantic/syntax, exit 3 for unsupported, exit 4 for runtime failure)
ENGINE_BIN="$REPO_ROOT/engine"
if [ -x "$ENGINE_BIN" ]; then
    echo "--- Running Error Tests (exit codes 2, 3, 4) ---"
    check_error() {
        local desc="$1"
        local cmd="$2"
        local expected_code="$3"
        local expected_str="$4"

        local err_output
        set +e
        err_output=$(eval "$cmd" 2>&1 >/dev/null)
        local code=$?
        set -e

        if [ "$code" -ne "$expected_code" ]; then
            echo "[FAIL] $desc: expected exit code $expected_code, got $code"
            FAILED=1
            return
        fi

        if ! echo "$err_output" | grep -Fiq "$expected_str"; then
            echo "[FAIL] $desc: expected stderr to contain '$expected_str', got: '$err_output'"
            FAILED=1
            return
        fi

        echo "[OK] $desc (exit $code)"
    }

    # Exit 2: Semantic / SQL errors
    check_error "Unknown column" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT nonexistent_column FROM t\"" \
        2 "unknown column"

    check_error "Unknown table" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM nonexistent_table\"" \
        2 "unknown table"

    check_error "Aggregate in WHERE" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t WHERE sum(qty) > 10\"" \
        2 "misuse of aggregate function"

    check_error "Unaggregated column in SELECT" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city, id FROM t GROUP BY city\"" \
        2 "must appear in the group by"

    check_error "Nested aggregate" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT sum(count(id)) FROM t\"" \
        2 "misuse of aggregate function"

    check_error "SELECT * in aggregate query" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT * FROM t GROUP BY city\"" \
        2 "SELECT * with GROUP BY or aggregation"

    check_error "Aggregate in GROUP BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t GROUP BY count(*)\"" \
        2 "aggregate functions are not allowed in GROUP BY"

    # Exit 3: Unsupported features
    check_error "GROUP BY ordinal" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t GROUP BY 1\"" \
        3 "not implemented yet: GROUP BY ordinal"

    check_error "COUNT(DISTINCT)" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT COUNT(DISTINCT city) FROM t\"" \
        3 "not implemented yet: COUNT(DISTINCT)"

    check_error "Scalar MIN with multiple arguments" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT min(1, 2) FROM t\"" \
        3 "not implemented yet: scalar function"

    check_error "HAVING on non-aggregate query" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t HAVING id > 5\"" \
        3 "not implemented yet: HAVING clause on a non-aggregate query"

    check_error "JOIN" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT t.id FROM t JOIN customers c ON t.id = c.id\"" \
        3 "not implemented yet: JOIN"

    check_error "ORDER BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t ORDER BY city\"" \
        3 "not implemented yet: ORDER BY"

    check_error "DISTINCT" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT DISTINCT city FROM t\"" \
        3 "not implemented yet: DISTINCT"

    # Exit 4: Runtime overflow / resource limits
    check_error "SUM 64-bit integer overflow" \
        "$ENGINE_BIN --table t='$DATA_DIR/big_values.csv' --sql \"SELECT SUM(val) FROM t\"" \
        4 "integer overflow"

    check_error "Max groups limit exceeded (--max-groups)" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city, COUNT(*) FROM t GROUP BY city\" --max-groups 1" \
        4 "maximum number of groups exceeded"
fi

# 5. Run compare.py against engine
if [ -x "$ENGINE_BIN" ]; then
    echo "--- Running engine comparisons against SQLite ---"
    
    # Test on 100k data
    echo "Testing on 100k dataset..."
    if ! "$PYTHON" tools/compare.py --csv "$DATA_100K" --engine "$ENGINE_BIN" --queries tests/queries.json; then
        FAILED=1
    fi
    
    # Test on edge files
    for edge_file in "$EDGE_DIR"/*.csv; do
        echo "Testing on edge file $(basename "$edge_file")..."
        if ! "$PYTHON" tools/compare.py --csv "$edge_file" --engine "$ENGINE_BIN" --queries tests/queries.json; then
            FAILED=1
        fi
    done

    # Test on text_sum dataset (mixed text and numeric prefixes)
    echo "Testing on text_sum dataset..."
    if ! "$PYTHON" tools/compare.py --csv "$DATA_DIR/text_sum.csv" --engine "$ENGINE_BIN" --queries tests/queries_text_sum.json; then
        FAILED=1
    fi

    # Test on nan_inf dataset (nan, inf treated as text)
    echo "Testing on nan_inf dataset..."
    if ! "$PYTHON" tools/compare.py --csv "$DATA_DIR/nan_inf.csv" --engine "$ENGINE_BIN" --queries tests/queries_nan_inf.json; then
        FAILED=1
    fi

    # Verify GROUP BY + LIMIT row count
    echo "Testing GROUP BY + LIMIT row count..."
    LIMIT_OUT=$("$ENGINE_BIN" --table t="$DATA_100K" --sql "SELECT city, COUNT(*) FROM t GROUP BY city LIMIT 5" | wc -l | tr -d ' ')
    if [ "$LIMIT_OUT" -ne 6 ]; then # 1 header line + 5 data rows
        echo "[FAIL] Expected 6 lines (header + 5 rows), got $LIMIT_OUT"
        FAILED=1
    else
        echo "[OK] GROUP BY + LIMIT row count verified (5 data rows)"
    fi
fi

# 6. Run parser tests (Phase 4) if test_parser.sh exists
if [ -f "$REPO_ROOT/tests/test_parser.sh" ]; then
    echo "--- Running Phase 4 Parser Tests ---"
    if ! bash "$REPO_ROOT/tests/test_parser.sh"; then
        FAILED=1
    fi
fi

# 7. Final summary
echo "==========================================="
if [ $FAILED -eq 0 ]; then
    echo "All tests PASSED!"
    exit 0
else
    echo "Some tests FAILED!"
    exit 1
fi

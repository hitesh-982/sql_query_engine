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

# 4. Run error tests (exit 2 for unknown table/col, exit 3 for unsupported features)
ENGINE_BIN="$REPO_ROOT/engine"
if [ -x "$ENGINE_BIN" ]; then
    echo "--- Running Error Tests (exit codes 2 and 3) ---"
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

        if ! echo "$err_output" | grep -qi "$expected_str"; then
            echo "[FAIL] $desc: expected stderr to contain '$expected_str', got: '$err_output'"
            FAILED=1
            return
        fi

        echo "[OK] $desc (exit $code)"
    }

    check_error "Unknown column" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT nonexistent_column FROM t\"" \
        2 "unknown column"

    check_error "Unknown table" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM nonexistent_table\"" \
        2 "unknown table"

    check_error "Aggregate in WHERE" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t WHERE sum(qty) > 10\"" \
        3 "not implemented yet: aggregate function"

    check_error "JOIN" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT t.id FROM t JOIN customers c ON t.id = c.id\"" \
        3 "not implemented yet: JOIN"

    check_error "GROUP BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t GROUP BY city\"" \
        3 "not implemented yet: GROUP BY"

    check_error "HAVING" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t HAVING 1 = 1\"" \
        3 "not implemented yet: HAVING"

    check_error "ORDER BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t ORDER BY city\"" \
        3 "not implemented yet: ORDER BY"

    check_error "DISTINCT" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT DISTINCT city FROM t\"" \
        3 "not implemented yet: DISTINCT"
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

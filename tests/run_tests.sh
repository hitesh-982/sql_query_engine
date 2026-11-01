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

if [ -x "./test_sort" ]; then
    echo "--- Running test_sort unit tests ---"
    if ./test_sort; then
        echo "[OK] test_sort passed"
    else
        echo "[FAIL] test_sort failed"
        FAILED=1
    fi
fi

if [ -x "./test_join" ]; then
    echo "--- Running test_join unit and property tests ---"
    if ./test_join; then
        echo "[OK] test_join passed"
    else
        echo "[FAIL] test_join failed"
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

# Generate join datasets and edge sets in tests/data/join_data
JOIN_DATA_DIR="$DATA_DIR/join_data"
if [ ! -f "$JOIN_DATA_DIR/orders.csv" ]; then
    echo "--- Generating join test datasets in $JOIN_DATA_DIR ---"
    "$PYTHON" tools/gen_data.py --join-tables "$JOIN_DATA_DIR" --orders 10000 --customers 1000 --products 100 --seed 42
    echo "[OK] Generated join tables in $JOIN_DATA_DIR"
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

        if ! echo "$err_output" | grep -Fiq -- "$expected_str"; then
            echo "[FAIL] $desc: expected stderr to contain '$expected_str', got: '$err_output'"
            FAILED=1
            return
        fi

        echo "[OK] $desc (exit $code)"
    }

    # Exit 1: Parse / Syntax errors
    check_error "OFFSET without LIMIT syntax error" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t OFFSET 5\"" \
        1 "unexpected token"

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

    check_error "ORDER BY unknown column" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY nonexistent_column\"" \
        2 "unknown column"

    check_error "ORDER BY 0 out of range" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY 0\"" \
        2 "ORDER BY term out of range"

    check_error "ORDER BY 99 out of range" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY 99\"" \
        2 "ORDER BY term out of range"

    check_error "SELECT DISTINCT with unselected ORDER BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT DISTINCT city FROM t ORDER BY id\"" \
        2 "for SELECT DISTINCT, ORDER BY terms must appear in the select list"

    check_error "ORDER BY bare column not in GROUP BY" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city FROM t GROUP BY city ORDER BY id\"" \
        2 "must appear in the group by"

    check_error "Aggregate in ORDER BY on non-aggregate query" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT 1 FROM t ORDER BY COUNT(*)\"" \
        2 "misuse of aggregate function in ORDER BY"

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

    # Phase 5: Unsupported join syntax (exit 3)
    check_error "RIGHT JOIN unsupported" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id\"" \
        3 "not implemented yet: RIGHT JOIN"

    check_error "FULL JOIN unsupported" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders o FULL JOIN customers c ON o.cust_id = c.id\"" \
        3 "not implemented yet: FULL JOIN"

    check_error "USING unsupported" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders o JOIN customers c USING (id)\"" \
        3 "not implemented yet: USING"

    check_error "NATURAL JOIN unsupported" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders o NATURAL JOIN customers c\"" \
        3 "not implemented yet: NATURAL JOIN"

    # Phase 5: Multi-table semantic errors (exit 2)
    check_error "Ambiguous column name" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT id FROM orders o JOIN customers c ON o.cust_id = c.id\"" \
        2 "ambiguous column name"

    check_error "Unknown table qualifier" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --sql \"SELECT x.id FROM orders o\"" \
        2 "unknown table qualifier"

    check_error "Alias hides table name" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --sql \"SELECT orders.id FROM orders o\"" \
        2 "hides table name"

    check_error "Duplicate table alias" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT o.id FROM orders o JOIN customers o ON o.cust_id = o.id\"" \
        2 "duplicate table name or alias"

    check_error "Duplicate table argument" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table orders='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT 1 FROM orders\"" \
        2 "duplicate table name"

    check_error "Aggregate in ON clause" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT o.id FROM orders o JOIN customers c ON count(*) = 1\"" \
        2 "misuse of aggregate function in ON"

    check_error "ON references later table" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --table products='$JOIN_DATA_DIR/products.csv' --sql \"SELECT o.id FROM orders o JOIN customers c ON o.cust_id = p.id JOIN products p ON o.product_id = p.id\"" \
        2 "unknown table qualifier"

    # Phase 5: Join parse errors (exit 1)
    check_error "Comma join parse error" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders, customers\"" \
        1 "unexpected token"

    check_error "Missing ON parse error" \
        "$ENGINE_BIN --table orders='$JOIN_DATA_DIR/orders.csv' --table customers='$JOIN_DATA_DIR/customers.csv' --sql \"SELECT * FROM orders JOIN customers\"" \
        1 "expected ON"

    # Exit 4: Runtime overflow / resource limits
    check_error "SUM 64-bit integer overflow" \
        "$ENGINE_BIN --table t='$DATA_DIR/big_values.csv' --sql \"SELECT SUM(val) FROM t\"" \
        4 "integer overflow"

    check_error "Max groups limit exceeded (--max-groups)" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT city, COUNT(*) FROM t GROUP BY city\" --max-groups 1" \
        4 "maximum number of groups exceeded"

    check_error "DISTINCT max groups limit exceeded (--max-groups)" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT DISTINCT city FROM t\" --max-groups 1" \
        4 "maximum number of groups exceeded"

    check_error "--memory-limit-mb 0 invalid" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY id\" --memory-limit-mb 0" \
        2 "--memory-limit-mb must be a positive integer"

    check_error "--memory-limit-mb -5 invalid" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY id\" --memory-limit-mb -5" \
        2 "--memory-limit-mb must be a positive integer"

    check_error "--sort-fan-in 1 invalid" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY id\" --sort-fan-in 1" \
        2 "--sort-fan-in must be an integer >= 2"

    check_error "External sort non-existent --tmp-dir" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY id\" --memory-limit-mb 1 --tmp-dir /nonexistent_dir_12345" \
        4 "failed to create temporary file in"

    READONLY_DIR="$REPO_ROOT/tests/data/readonly_tmp_test"
    mkdir -p "$READONLY_DIR"
    chmod 555 "$READONLY_DIR"
    check_error "External sort read-only --tmp-dir" \
        "$ENGINE_BIN --table t='$DATA_100K' --sql \"SELECT id FROM t ORDER BY id\" --memory-limit-mb 1 --tmp-dir '$READONLY_DIR'" \
        4 "failed to create temporary file in"
    chmod 755 "$READONLY_DIR"
    rm -rf "$READONLY_DIR"
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

    # Test external sort (force spill with --memory-limit-mb 1 --sort-fan-in 4)
    echo "Testing external merge sort (--memory-limit-mb 1) on 100k dataset..."
    if ! "$PYTHON" tools/compare.py --csv "$DATA_100K" --engine "$ENGINE_BIN" --queries tests/queries_external_sort.json; then
        FAILED=1
    fi

    # Test temp file cleanup (no leftover temp files after query)
    echo "Testing external sort temp file cleanup..."
    CLEANUP_TMP_DIR=$(mktemp -d)
    "$ENGINE_BIN" --table t="$DATA_100K" --sql "SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 2000" --memory-limit-mb 1 --tmp-dir "$CLEANUP_TMP_DIR" --no-output
    LEFTOVER_COUNT=$(ls -A "$CLEANUP_TMP_DIR" | wc -l)
    rm -rf "$CLEANUP_TMP_DIR"
    if [ "$LEFTOVER_COUNT" -ne 0 ]; then
        echo "[FAIL] Leftover temp files found in tmp dir: $LEFTOVER_COUNT"
        FAILED=1
    else
        echo "[OK] No leftover temp files in tmp-dir"
    fi

    # Scale verification under ulimit -n 256 (if mid.csv or data_5m.csv is present)
    SCALE_CSV=""
    if [ -f "$REPO_ROOT/mid.csv" ]; then
        SCALE_CSV="$REPO_ROOT/mid.csv"
    elif [ -f "$REPO_ROOT/tests/data/data_5m.csv" ]; then
        SCALE_CSV="$REPO_ROOT/tests/data/data_5m.csv"
    fi
    if [ -n "$SCALE_CSV" ]; then
        echo "Testing 5M row external sort under ulimit -n 256 on $SCALE_CSV..."
        (
            ulimit -n 256
            TMP_SCALE_DIR=$(mktemp -d)
            trap 'rm -rf "$TMP_SCALE_DIR"' EXIT
            
            # Query 1: SELECT id, city, amount FROM t ORDER BY amount DESC, id
            echo "  Scale query 1: id, city, amount ORDER BY amount DESC, id..."
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT id, city, amount FROM t ORDER BY amount DESC, id" > "$TMP_SCALE_DIR/q1_inmem.out"
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT id, city, amount FROM t ORDER BY amount DESC, id" --memory-limit-mb 1 --tmp-dir "$TMP_SCALE_DIR" --sort-stats > "$TMP_SCALE_DIR/q1_ext.out"
            diff -q "$TMP_SCALE_DIR/q1_inmem.out" "$TMP_SCALE_DIR/q1_ext.out"
            
            # Query 2: SELECT city, id FROM t ORDER BY city, id
            echo "  Scale query 2: city, id ORDER BY city, id..."
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT city, id FROM t ORDER BY city, id" > "$TMP_SCALE_DIR/q2_inmem.out"
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT city, id FROM t ORDER BY city, id" --memory-limit-mb 1 --tmp-dir "$TMP_SCALE_DIR" --sort-stats > "$TMP_SCALE_DIR/q2_ext.out"
            diff -q "$TMP_SCALE_DIR/q2_inmem.out" "$TMP_SCALE_DIR/q2_ext.out"

            # Query 3: SELECT id, amount FROM t ORDER BY qty DESC, id (hidden column)
            echo "  Scale query 3: id, amount ORDER BY qty DESC, id (hidden sort col)..."
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT id, amount FROM t ORDER BY qty DESC, id" > "$TMP_SCALE_DIR/q3_inmem.out"
            "$ENGINE_BIN" --table t="$SCALE_CSV" --sql "SELECT id, amount FROM t ORDER BY qty DESC, id" --memory-limit-mb 1 --tmp-dir "$TMP_SCALE_DIR" --sort-stats > "$TMP_SCALE_DIR/q3_ext.out"
            diff -q "$TMP_SCALE_DIR/q3_inmem.out" "$TMP_SCALE_DIR/q3_ext.out"
            echo "[OK] 5M rows under ulimit -n 256: all 3 queries byte-for-byte identical to in-memory!"
        ) || {
            echo "[FAIL] 5M row external sort under ulimit -n 256 failed!"
            FAILED=1
        }
    fi

    # Phase 5: Join comparisons against SQLite
    echo "--- Running Phase 5 Join Comparisons against SQLite ---"
    echo "Testing 130 join queries on base dataset..."
    if ! "$PYTHON" tools/compare.py \
        --table orders="$JOIN_DATA_DIR/orders.csv" \
        --table customers="$JOIN_DATA_DIR/customers.csv" \
        --table products="$JOIN_DATA_DIR/products.csv" \
        --engine "$ENGINE_BIN" \
        --queries tests/queries_join.json; then
        FAILED=1
    fi

    # Test on all 7 join edge sets
    JOIN_EDGE_DIR="$JOIN_DATA_DIR/edge"
    for edge_subdir in "$JOIN_EDGE_DIR"/*; do
        if [ -d "$edge_subdir" ]; then
            edge_name=$(basename "$edge_subdir")
            echo "Testing join queries on edge set $edge_name..."
            p_file="$edge_subdir/products.csv"
            if [ ! -f "$p_file" ]; then
                p_file="$JOIN_DATA_DIR/products.csv"
            fi
            if ! "$PYTHON" tools/compare.py \
                --table orders="$edge_subdir/orders.csv" \
                --table customers="$edge_subdir/customers.csv" \
                --table products="$p_file" \
                --engine "$ENGINE_BIN" \
                --queries tests/queries_join.json; then
                FAILED=1
            fi
        fi
    done

    # Self-check: verify identical output with and without --no-pushdown
    echo "Testing filter pushdown self-check (with vs without --no-pushdown)..."
    PUSHDOWN_Q="SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 500 AND c.region = 'North' ORDER BY o.id, c.name"
    TMP_PD_DIR=$(mktemp -d)
    "$ENGINE_BIN" --table orders="$JOIN_DATA_DIR/orders.csv" --table customers="$JOIN_DATA_DIR/customers.csv" --sql "$PUSHDOWN_Q" > "$TMP_PD_DIR/with_pushdown.out"
    "$ENGINE_BIN" --table orders="$JOIN_DATA_DIR/orders.csv" --table customers="$JOIN_DATA_DIR/customers.csv" --no-pushdown --sql "$PUSHDOWN_Q" > "$TMP_PD_DIR/without_pushdown.out"
    if ! diff -q "$TMP_PD_DIR/with_pushdown.out" "$TMP_PD_DIR/without_pushdown.out"; then
        echo "[FAIL] Pushdown outputs differ!"
        FAILED=1
    else
        echo "[OK] Filter pushdown self-check: outputs are byte-for-byte identical!"
    fi
    rm -rf "$TMP_PD_DIR"

    # Scale test: 1M orders / 20k customers verification against SQLite
    echo "Testing 1M orders / 20k customers scale join against SQLite..."
    SCALE_JOIN_DIR=$(mktemp -d)
    "$PYTHON" tools/gen_data.py --join-tables "$SCALE_JOIN_DIR" --orders 1000000 --customers 20000 --products 1000 --no-edges
    SCALE_Q_JSON="$SCALE_JOIN_DIR/scale_queries.json"
    cat << 'EOF' > "$SCALE_Q_JSON"
[
  {
    "name": "scale_1m_join_group_by",
    "sql": "SELECT c.region, count(*), sum(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region",
    "ordered": true
  },
  {
    "name": "scale_1m_join_anti",
    "sql": "SELECT count(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL",
    "ordered": false
  }
]
EOF

    if ! "$PYTHON" tools/compare.py \
        --table orders="$SCALE_JOIN_DIR/orders.csv" \
        --table customers="$SCALE_JOIN_DIR/customers.csv" \
        --table products="$SCALE_JOIN_DIR/products.csv" \
        --engine "$ENGINE_BIN" \
        --queries "$SCALE_Q_JSON"; then
        echo "[FAIL] 1M orders / 20k customers scale join failed!"
        FAILED=1
    else
        echo "[OK] 1M orders / 20k customers scale join matches SQLite exactly!"
    fi
    rm -rf "$SCALE_JOIN_DIR"
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

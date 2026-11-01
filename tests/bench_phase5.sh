#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ENGINE_BIN="$REPO_ROOT/engine"
if [ ! -x "$ENGINE_BIN" ]; then
    echo "Building engine..."
    make engine
fi

PYTHON="python3"
if [ -x "/usr/bin/python3" ]; then
    PYTHON="/usr/bin/python3"
fi

BENCH_DIR="${TMPDIR:-/tmp}/bench_phase5_$$"
mkdir -p "$BENCH_DIR"
trap 'rm -rf "$BENCH_DIR"' EXIT

ORDERS_ROWS=5000000
CUSTOMERS_ROWS=100000
PRODUCTS_ROWS=1000

echo "=== Phase 5 Join Benchmark ==="
echo "Generating synthetic benchmark datasets ($ORDERS_ROWS orders, $CUSTOMERS_ROWS customers, $PRODUCTS_ROWS products)..."
"$PYTHON" tools/gen_data.py --join-tables "$BENCH_DIR" \
    --orders "$ORDERS_ROWS" --customers "$CUSTOMERS_ROWS" --products "$PRODUCTS_ROWS" --no-edges

ORDERS_CSV="$BENCH_DIR/orders.csv"
CUSTOMERS_CSV="$BENCH_DIR/customers.csv"
PRODUCTS_CSV="$BENCH_DIR/products.csv"

echo "Datasets generated:"
ls -lh "$ORDERS_CSV" "$CUSTOMERS_CSV" "$PRODUCTS_CSV"
echo ""

run_bench_query() {
    local q_id="$1"
    local desc="$2"
    local cmd="$3"
    local out_file="$4"

    echo "Running Query $q_id: $desc..." >&2
    local start_ns
    local end_ns
    start_ns=$(date +%s%N)
    
    if [ -n "$out_file" ]; then
        eval "$cmd" > "$out_file"
    else
        eval "$cmd" > /dev/null
    fi
    
    end_ns=$(date +%s%N)
    local elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
    local elapsed_sec=$(awk "BEGIN {printf \"%.3f\", $elapsed_ms / 1000}")
    echo "  -> Completed in ${elapsed_sec}s" >&2
    echo "$elapsed_sec"
}

Q2_OUT="$BENCH_DIR/q2.out"
Q3_OUT="$BENCH_DIR/q3.out"
PUSHDOWN_YES_OUT="$BENCH_DIR/pushdown_yes.out"
PUSHDOWN_NO_OUT="$BENCH_DIR/pushdown_no.out"

echo "--------------------------------------------------------------------------------"
# Query 1: Scan baseline
T1=$(run_bench_query 1 "Scan baseline (SELECT COUNT(*) FROM orders)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --sql 'SELECT COUNT(*) FROM orders'" "")

# Query 2: Standard INNER join (orders o JOIN customers c)
T2=$(run_bench_query 2 "Standard INNER join (orders o JOIN customers c)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --sql 'SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id LIMIT 1000'" "$Q2_OUT")

# Query 3: Swapped candidate INNER join (customers c JOIN orders o)
T3=$(run_bench_query 3 "Swapped candidate INNER join (customers c JOIN orders o)" \
    "$ENGINE_BIN --table customers='$CUSTOMERS_CSV' --table orders='$ORDERS_CSV' --sql 'SELECT o.id, c.name, o.amount FROM customers c JOIN orders o ON c.id = o.cust_id ORDER BY o.id LIMIT 1000'" "$Q3_OUT")

# Verification: cmp Q2 and Q3
CMP_Q2_Q3="FAIL"
if cmp -s "$Q2_OUT" "$Q3_OUT"; then
    CMP_Q2_Q3="IDENTICAL (PASS)"
    echo "  [VERIFY] Query 2 and Query 3 outputs are byte-for-byte IDENTICAL!"
else
    echo "  [VERIFY] Query 2 and Query 3 outputs differ!"
fi

# Query 4: GROUP BY over join
T4=$(run_bench_query 4 "GROUP BY over join (region, count, sum)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --sql 'SELECT c.region, COUNT(*), SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region'" "")

# Query 5: Anti-join (LEFT JOIN WHERE c.id IS NULL)
T5=$(run_bench_query 5 "Anti-join (LEFT JOIN ... WHERE c.id IS NULL)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --sql 'SELECT COUNT(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL'" "")

# Query 6: 3-table join
T6=$(run_bench_query 6 "3-table join (orders -> customers -> products)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --table products='$PRODUCTS_CSV' --sql 'SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id'" "")

# Query 7a: Pushdown enabled
T7A=$(run_bench_query "7a" "Pushdown ENABLED (selective WHERE o.amount > 5000)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --sql 'SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 5000 ORDER BY o.id LIMIT 500'" "$PUSHDOWN_YES_OUT")

# Query 7b: Pushdown disabled (--no-pushdown)
T7B=$(run_bench_query "7b" "Pushdown DISABLED (--no-pushdown)" \
    "$ENGINE_BIN --table orders='$ORDERS_CSV' --table customers='$CUSTOMERS_CSV' --no-pushdown --sql 'SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 5000 ORDER BY o.id LIMIT 500'" "$PUSHDOWN_NO_OUT")

CMP_PUSHDOWN="FAIL"
if cmp -s "$PUSHDOWN_YES_OUT" "$PUSHDOWN_NO_OUT"; then
    CMP_PUSHDOWN="IDENTICAL (PASS)"
    echo "  [VERIFY] Pushdown enabled and disabled outputs are byte-for-byte IDENTICAL!"
fi

echo ""
echo "=== Phase 5 Benchmark Summary Table ==="
printf "%-8s | %-45s | %-12s | %-20s\n" "Query" "Description" "Time (s)" "Verification"
echo "---------+-----------------------------------------------+--------------+---------------------"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q1" "Scan baseline (SELECT COUNT(*))" "${T1}s" "N/A"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q2" "Standard INNER join" "${T2}s" "Matches Q3: ${CMP_Q2_Q3}"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q3" "Swapped INNER join (customers JOIN orders)" "${T3}s" "Matches Q2: ${CMP_Q2_Q3}"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q4" "GROUP BY over join" "${T4}s" "N/A"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q5" "Anti-join (LEFT JOIN ... WHERE c.id IS NULL)" "${T5}s" "N/A"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q6" "3-table join (orders + cust + prod)" "${T6}s" "N/A"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q7a" "Pushdown enabled" "${T7A}s" "Matches Q7b: ${CMP_PUSHDOWN}"
printf "%-8s | %-45s | %-12s | %-20s\n" "Q7b" "Pushdown disabled (--no-pushdown)" "${T7B}s" "Matches Q7a: ${CMP_PUSHDOWN}"
echo "================================================================================"


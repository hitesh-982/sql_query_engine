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

BENCH_DIR="${TMPDIR:-/tmp}/bench_phase5b_$$"
mkdir -p "$BENCH_DIR"
trap 'rm -rf "$BENCH_DIR"' EXIT

ORDERS_ROWS=5000000
CUSTOMERS_ROWS=100000
PRODUCTS_ROWS=1000

echo "=== Phase 5B Outer Join Benchmark ==="
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
    local sql="$3"
    local out_file="$4"

    echo "================================================================================"
    echo "Running Query $q_id: $desc"
    echo "SQL: $sql"
    echo "--- Explain Plan ---"
    "$ENGINE_BIN" --table orders="$ORDERS_CSV" --table customers="$CUSTOMERS_CSV" --sql "$sql" --explain
    echo "--- Join Stats & Execution ---"

    local time_err="$BENCH_DIR/time_err_$q_id.tmp"
    local stats_out="$BENCH_DIR/stats_$q_id.tmp"
    if [ -n "$out_file" ]; then
        eval "$PYTHON \"$REPO_ROOT/tools/time_cmd.py\" \"$ENGINE_BIN\" --table orders=\"$ORDERS_CSV\" --table customers=\"$CUSTOMERS_CSV\" --sql \"$sql\" --join-stats" > "$out_file" 2> "$time_err"
    else
        eval "$PYTHON \"$REPO_ROOT/tools/time_cmd.py\" \"$ENGINE_BIN\" --table orders=\"$ORDERS_CSV\" --table customers=\"$CUSTOMERS_CSV\" --sql \"$sql\" --join-stats --no-output" > "$stats_out" 2> "$time_err"
        cat "$stats_out"
        rm -f "$stats_out"
    fi

    local time_info
    time_info=$(cat "$time_err")
    rm -f "$time_err"
    echo "Timing & Memory: $time_info"

    local elapsed_sec
    elapsed_sec=$(echo "$time_info" | awk -F'|' '{print $1}' | sed 's/Elapsed: //; s/ s//; s/ //g')
    local peak_rss
    peak_rss=$(echo "$time_info" | awk -F'|' '{print $2}' | sed 's/Peak RSS: //; s/ //g')
    echo "${elapsed_sec}|${peak_rss}"
}

Q1_OUT="$BENCH_DIR/q1.out"
Q2_OUT="$BENCH_DIR/q2.out"
Q3_OUT="$BENCH_DIR/q3.out"
Q4_OUT="$BENCH_DIR/q4.out"
Q5_OUT="$BENCH_DIR/q5.out"
Q_LEFT_OUT="$BENCH_DIR/q_left.out"

# Query 1: orders o RIGHT JOIN customers c
RES1=$(run_bench_query 1 "orders o RIGHT JOIN customers c" \
    "SELECT COUNT(*), COUNT(c.id) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id" "$Q1_OUT")
T1=$(echo "$RES1" | cut -d'|' -f1)
RSS1=$(echo "$RES1" | cut -d'|' -f2)

# Query 2: customers c RIGHT JOIN orders o (swapped optimization active)
RES2=$(run_bench_query 2 "customers c RIGHT JOIN orders o (swapped: probe=orders, build=customers)" \
    "SELECT COUNT(*), COUNT(o.id) FROM customers c RIGHT JOIN orders o ON o.cust_id = c.id" "$Q2_OUT")
T2=$(echo "$RES2" | cut -d'|' -f1)
RSS2=$(echo "$RES2" | cut -d'|' -f2)

# Query 3: orders o FULL JOIN customers c
RES3=$(run_bench_query 3 "orders o FULL JOIN customers c" \
    "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id" "$Q3_OUT")
T3=$(echo "$RES3" | cut -d'|' -f1)
RSS3=$(echo "$RES3" | cut -d'|' -f2)

# Query 4: Anti-join: orders o RIGHT JOIN customers c WHERE o.id IS NULL
RES4=$(run_bench_query 4 "Anti-join: orders o RIGHT JOIN customers c WHERE o.id IS NULL" \
    "SELECT COUNT(*) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL" "$Q4_OUT")
T4=$(echo "$RES4" | cut -d'|' -f1)
RSS4=$(echo "$RES4" | cut -d'|' -f2)

# Query 5: customers c FULL JOIN orders o
RES5=$(run_bench_query 5 "customers c FULL JOIN orders o" \
    "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM customers c FULL JOIN orders o ON c.id = o.cust_id" "$Q5_OUT")
T5=$(echo "$RES5" | cut -d'|' -f1)
RSS5=$(echo "$RES5" | cut -d'|' -f2)

# Baseline LEFT join for verification
"$ENGINE_BIN" --table orders="$ORDERS_CSV" --table customers="$CUSTOMERS_CSV" \
    --sql "SELECT COUNT(*), COUNT(o.id) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id" > "$Q_LEFT_OUT"

echo "================================================================================"
echo "--- Verifications ---"

# 1. Q2 counts equal orders o LEFT JOIN customers c
CMP_Q2_LEFT="FAIL"
if cmp -s "$Q2_OUT" "$Q_LEFT_OUT"; then
    CMP_Q2_LEFT="PASS"
    echo "  [PASS] Query 2 counts equal orders o LEFT JOIN customers c:"
    cat "$Q2_OUT"
else
    echo "  [FAIL] Query 2 does not match orders LEFT JOIN customers!"
    echo "    Q2: $(cat "$Q2_OUT")"
    echo "    LEFT: $(cat "$Q_LEFT_OUT")"
fi

# 2. Q3 counts match Q5
CMP_Q3_Q5="FAIL"
if cmp -s "$Q3_OUT" "$Q5_OUT"; then
    CMP_Q3_Q5="PASS"
    echo "  [PASS] Query 3 counts equal Query 5 counts:"
    cat "$Q3_OUT"
else
    echo "  [FAIL] Query 3 counts differ from Query 5 counts!"
    echo "    Q3: $(cat "$Q3_OUT")"
    echo "    Q5: $(cat "$Q5_OUT")"
fi

# 3. Q3 count equals LEFT join count + unmatched customers (Q4)
Q3_TOTAL=$(tail -n 1 "$Q3_OUT" | cut -d',' -f1)
LEFT_TOTAL=$(tail -n 1 "$Q_LEFT_OUT" | cut -d',' -f1)
UNMATCHED_CUST=$(tail -n 1 "$Q4_OUT")
EXPECTED_Q3_TOTAL=$((LEFT_TOTAL + UNMATCHED_CUST))
CMP_Q3_MATH="FAIL"
if [ "$Q3_TOTAL" -eq "$EXPECTED_Q3_TOTAL" ]; then
    CMP_Q3_MATH="PASS"
    echo "  [PASS] Query 3 total ($Q3_TOTAL) == LEFT join total ($LEFT_TOTAL) + unmatched customers ($UNMATCHED_CUST)"
else
    echo "  [FAIL] Query 3 total ($Q3_TOTAL) != LEFT total ($LEFT_TOTAL) + unmatched ($UNMATCHED_CUST)"
fi

echo ""
echo "=== Phase 5B Benchmark Summary Table ==="
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Query" "Description" "Time (s)" "Peak RSS" "Verification"
echo "---------+-----------------------------------------------+------------+------------+---------------------"
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Q1" "orders RIGHT JOIN customers" "${T1}s" "$RSS1" "Output rows: $(tail -n 1 "$Q1_OUT")"
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Q2" "customers RIGHT JOIN orders (swapped)" "${T2}s" "$RSS2" "Matches LEFT: ${CMP_Q2_LEFT}"
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Q3" "orders FULL JOIN customers" "${T3}s" "$RSS3" "Matches Q5: ${CMP_Q3_Q5}"
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Q4" "Unmatched customers (anti-join)" "${T4}s" "$RSS4" "Count: ${UNMATCHED_CUST}"
printf "%-8s | %-45s | %-10s | %-10s | %-20s\n" "Q5" "customers FULL JOIN orders" "${T5}s" "$RSS5" "Math check: ${CMP_Q3_MATH}"
echo "=========================================================================================="


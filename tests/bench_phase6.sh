#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ -x "/usr/bin/python3" ]; then
    PYTHON="/usr/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
else
    PYTHON="python"
fi

if [ -f "$REPO_ROOT/mid.csv" ]; then
    DATA_5M="$REPO_ROOT/mid.csv"
else
    DATA_5M="$REPO_ROOT/tests/data/data_5m.csv"
fi
if [ ! -f "$DATA_5M" ]; then
    echo "--- Generating 5M-row dataset ($DATA_5M) ---"
    "$PYTHON" tools/gen_data.py --rows 5000000 --out "$DATA_5M" --seed 42
    echo "[OK] Generated $DATA_5M"
fi

ENGINE_BIN="$REPO_ROOT/engine"
if [ ! -x "$ENGINE_BIN" ]; then
    echo "--- Building engine ---"
    make engine
fi

# Robust timing helper using Python's resource module (measures child RSS and wall-clock time)
TIME_CMD="$PYTHON tools/time_cmd.py"
parse_time_rss() {
    local out_err="$1"
    cat "$out_err"
}

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== Phase 6 Benchmarks (5 Million Rows) ==="

echo "0. Baseline Scan: SELECT COUNT(*) FROM t"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT COUNT(*) FROM t" --no-output 2> "$TMP_DIR/baseline.time"
parse_time_rss "$TMP_DIR/baseline.time"

echo "1. Top-N: SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 10"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 10" > "$TMP_DIR/top10.out" 2> "$TMP_DIR/top10.time"
parse_time_rss "$TMP_DIR/top10.time"

echo "2. Full Sort: SELECT id, amount FROM t ORDER BY amount DESC, id"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT id, amount FROM t ORDER BY amount DESC, id" > "$TMP_DIR/full.out" 2> "$TMP_DIR/full.time"
parse_time_rss "$TMP_DIR/full.time"

echo "3. Distinct: SELECT DISTINCT city FROM t"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT DISTINCT city FROM t" --no-output 2> "$TMP_DIR/distinct.time"
parse_time_rss "$TMP_DIR/distinct.time"

echo "4. Aggregate + Sort + Limit: SELECT city, COUNT(*) AS n FROM t GROUP BY city ORDER BY n DESC, city LIMIT 5"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT city, COUNT(*) AS n FROM t GROUP BY city ORDER BY n DESC, city LIMIT 5" > "$TMP_DIR/agg.out" 2> "$TMP_DIR/agg.time"
parse_time_rss "$TMP_DIR/agg.time"

echo "5. External Sort (--memory-limit-mb 64): SELECT id, amount FROM t ORDER BY amount DESC, id"
$TIME_CMD "$ENGINE_BIN" --table t="$DATA_5M" --sql "SELECT id, amount FROM t ORDER BY amount DESC, id" --memory-limit-mb 64 --sort-stats --no-output 2> "$TMP_DIR/ext_sort.time"
parse_time_rss "$TMP_DIR/ext_sort.time"

echo "--- Verifying Top-10 output matches first 10 data rows of full sort ---"
tail -n +2 "$TMP_DIR/top10.out" | head -n 10 > "$TMP_DIR/top10_data.txt"
tail -n +2 "$TMP_DIR/full.out" | head -n 10 > "$TMP_DIR/full_first10.txt"
if diff -q "$TMP_DIR/top10_data.txt" "$TMP_DIR/full_first10.txt" >/dev/null; then
    echo "[PASS] Top-10 output exactly matches the first 10 rows of Full Sort!"
else
    echo "[FAIL] Mismatch between Top-10 and Full Sort first 10 rows!"
    diff -u "$TMP_DIR/top10_data.txt" "$TMP_DIR/full_first10.txt"
    exit 1
fi

echo "=== Benchmark Complete ==="

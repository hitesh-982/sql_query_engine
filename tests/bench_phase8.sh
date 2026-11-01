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

BENCH_DIR="${TMPDIR:-/tmp}/bench_phase8_$$"
mkdir -p "$BENCH_DIR"
trap 'rm -rf "$BENCH_DIR"' EXIT

echo "=== Phase 8 Performance Benchmark ==="
echo "Machine status:"
uptime
echo ""

echo "Generating 5M single-table dataset (seed 2)..."
"$PYTHON" tools/gen_data.py --rows 5000000 --out "$BENCH_DIR/t.csv" --seed 2

echo "Generating 5M join dataset: 5M orders, 100k customers, 1k products (seed 5)..."
"$PYTHON" tools/gen_data.py --join-tables "$BENCH_DIR/j" --orders 5000000 --customers 100000 --products 1000 --seed 5 --no-edges

echo "Datasets generated successfully:"
ls -lh "$BENCH_DIR/t.csv" "$BENCH_DIR/j"/*.csv
echo ""

echo "Running benchmark queries (5 iterations per query)..."
"$PYTHON" tools/bench_runner.py \
    --engine "$ENGINE_BIN" \
    --time-cmd "$REPO_ROOT/tools/time_cmd.py" \
    --single-csv "$BENCH_DIR/t.csv" \
    --join-dir "$BENCH_DIR/j" \
    "$@"


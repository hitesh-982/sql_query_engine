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
    echo "[INFO] No Makefile present yet (Phase 0)."
fi

# 2. Generate 100k-row test dataset and edge datasets
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

# 3. Run compare.py against engine if engine binary exists
ENGINE_BIN="$REPO_ROOT/engine"
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
else
    echo "[INFO] Engine binary ($ENGINE_BIN) not built yet (Phase 0). Skipping CSV scan comparison."
fi

# 4. Run parser tests (Phase 4) if test_parser.sh exists
if [ -f "$REPO_ROOT/tests/test_parser.sh" ]; then
    echo "--- Running Phase 4 Parser Tests ---"
    if ! bash "$REPO_ROOT/tests/test_parser.sh"; then
        FAILED=1
    fi
fi

# 5. Final summary
echo "==========================================="
if [ $FAILED -eq 0 ]; then
    echo "All tests PASSED!"
    exit 0
else
    echo "Some tests FAILED!"
    exit 1
fi

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

echo "--- 1. Lexer Unit Tests ---"
./test_lexer

echo "--- 2. Parser Positive, Negative & Round-Trip Tests ---"
"$PYTHON" tools/run_parser_tests.py

echo "--- 3. Parser Fuzz Test (Sanitized) ---"
"$PYTHON" tools/fuzz_parser.py --count 1000

echo "[OK] All Phase 4 parser tests passed!"

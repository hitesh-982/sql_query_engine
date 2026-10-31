#!/usr/bin/env python3
"""
fuzz_parser.py - Fuzz testing for the SQL parser.
Feeds 10,000 random token sequences and mutations of valid queries to ./sqlparse_sanitized.
Must never hang or crash, exiting strictly with 0 or 1.
"""

import argparse
import concurrent.futures
import os
import random
import string
import subprocess
import sys

BASE_TOKENS = [
    "SELECT", "DISTINCT", "FROM", "WHERE", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "ON",
    "GROUP", "BY", "HAVING", "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "AND", "OR", "NOT", "IS",
    "NULL", "IN", "BETWEEN", "LIKE", "AS", "COUNT", "SUM", "AVG", "MIN", "MAX",
    "t", "u", "v", "a", "b", "c", "city", "qty", "amount", "id", "t.a", "u.id", "*", "t.*",
    ",", ".", ";", "(", ")", "+", "-", "*", "/", "=", "!=", "<>", "<", "<=", ">", ">=",
    "0", "1", "42", "-1", "100", "0.5", "3.14", "1e5", "99999999999999999999",
    "'hello'", "''", "'don''t'", "\"quoted col\"", "\"my table\"",
    "-- comment\n", "/* block */"
]

SAMPLE_QUERIES = [
    "SELECT * FROM t;",
    "SELECT a, b FROM t WHERE a > 5;",
    "SELECT DISTINCT city, sum(amount) FROM t GROUP BY city HAVING sum(amount) > 100;",
    "SELECT t.id, u.name FROM t LEFT JOIN u ON t.id = u.id ORDER BY t.id DESC LIMIT 10 OFFSET 5;",
    "SELECT a FROM t WHERE a BETWEEN 10 AND 20 AND b IN ('x', 'y') OR c IS NOT NULL;",
    "SELECT count(*), count(DISTINCT city) FROM t;",
    "SELECT 1 + 2 * 3 - 4 / 2 FROM t;"
]


def generate_random_token_sequence():
    length = random.randint(1, 30)
    toks = [random.choice(BASE_TOKENS) for _ in range(length)]
    return " ".join(toks)


def generate_mutated_query():
    base = random.choice(SAMPLE_QUERIES)
    toks = base.split()
    mut_type = random.randint(0, 4)
    if mut_type == 0 and len(toks) > 1:
        # Delete token
        del toks[random.randint(0, len(toks) - 1)]
    elif mut_type == 1:
        # Insert random token
        toks.insert(random.randint(0, len(toks)), random.choice(BASE_TOKENS))
    elif mut_type == 2 and len(toks) > 0:
        # Replace token
        toks[random.randint(0, len(toks) - 1)] = random.choice(BASE_TOKENS)
    elif mut_type == 3:
        # Random character injection
        raw = " ".join(toks)
        idx = random.randint(0, len(raw))
        char = random.choice(string.punctuation + "0123456789abc ")
        raw = raw[:idx] + char + raw[idx:]
        return raw
    elif mut_type == 4:
        # Parens flood
        raw = "SELECT " + "(" * random.randint(1, 250) + "1" + ")" * random.randint(1, 250) + " FROM t;"
        return raw
    return " ".join(toks)


def run_single_fuzz(bin_path, query):
    try:
        proc = subprocess.run(
            [bin_path, query],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2.0
        )
        if proc.returncode not in (0, 1):
            return (False, query, proc.returncode, proc.stderr)
        return (True, query, proc.returncode, "")
    except subprocess.TimeoutExpired:
        return (False, query, -1, "Timeout (2s expired)")
    except Exception as e:
        return (False, query, -2, str(e))


def main():
    parser = argparse.ArgumentParser(description="Fuzz test for SQL parser")
    parser.add_argument("--count", type=int, default=10000, help="Number of fuzz iterations")
    parser.add_argument("--bin", type=str, default=None, help="Path to sqlparse binary")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--workers", type=int, default=8, help="Parallel worker threads")
    args = parser.parse_args()

    random.seed(args.seed)

    bin_path = args.bin
    if not bin_path:
        if os.path.isfile("./sqlparse_sanitized"):
            bin_path = "./sqlparse_sanitized"
        elif os.path.isfile("./sqlparse"):
            bin_path = "./sqlparse"
        else:
            print("Error: neither ./sqlparse_sanitized nor ./sqlparse found. Please build first.", file=sys.stderr)
            sys.exit(1)

    print(f"=== Starting SQL Parser Fuzz Test ({args.count} queries using {bin_path}) ===")

    # Generate test cases
    queries = []
    half = args.count // 2
    for _ in range(half):
        queries.append(generate_random_token_sequence())
    for _ in range(args.count - half):
        queries.append(generate_mutated_query())

    failed = 0
    completed = 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = [executor.submit(run_single_fuzz, bin_path, q) for q in queries]
        for f in concurrent.futures.as_completed(futures):
            ok, q, code, err = f.result()
            completed += 1
            if not ok:
                failed += 1
                print(f"[FAIL] Query caused hang or crash (exit code {code}):\n  SQL: {q}\n  stderr: {err}")
                if failed >= 5:
                    print("Too many failures, aborting.")
                    break
            if completed % 2500 == 0:
                print(f"  Progress: {completed}/{args.count} completed...")

    if failed == 0:
        print(f"=== Fuzz Testing PASSED: {completed} queries processed with 0 crashes or hangs! ===")
        sys.exit(0)
    else:
        print(f"=== Fuzz Testing FAILED: {failed} queries failed! ===")
        sys.exit(1)


if __name__ == "__main__":
    main()

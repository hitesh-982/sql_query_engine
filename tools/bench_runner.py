#!/usr/bin/env python3
"""
bench_runner.py - Benchmark runner for Phase 8.
Runs benchmark queries 5 times, extracts min/median/max elapsed time and peak RSS,
computes output SHA256, prints progress, and outputs TSV.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

QUERIES_SINGLE = [
    ("S1", "SELECT COUNT(*) FROM t", True, "scan floor"),
    ("S2", "SELECT COUNT(*) FROM t WHERE qty > 5 AND amount < 100", True, "parse + filter"),
    ("S3", "SELECT SUM(amount), AVG(qty), MIN(id), MAX(id) FROM t", True, "numeric parse + agg"),
    ("S4", "SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city", True, "string-key group"),
    ("S5", "SELECT region, qty, COUNT(*) FROM t GROUP BY region, qty", True, "multi-key group"),
    ("S6", "SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 10", True, "top-N"),
    ("S7", "SELECT id, amount FROM t ORDER BY amount DESC, id", True, "full sort, output"),
    ("S8", "SELECT DISTINCT city FROM t", True, "distinct"),
    ("S9", "SELECT COUNT(*) FROM t WHERE note LIKE '%urgent%'", True, "string op, quoted col"),
    ("S10", "SELECT id, city, amount FROM t", False, "output formatting"),
]

QUERIES_JOIN = [
    ("J1", "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id", True, "inner join count"),
    ("J2", "SELECT c.region, COUNT(*), SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region", True, "join + group by"),
    ("J3", "SELECT c.name, COUNT(*) AS n FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id WHERE p.category = 'x' GROUP BY c.name ORDER BY n DESC, c.name LIMIT 10", True, "3-table join + agg + top-N"),
    ("J4", "SELECT COUNT(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL", True, "left anti-join"),
    ("J5", "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id", False, "join output to devnull"),
    ("J6", "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id", True, "full outer join"),
]

TIME_CMD_RE = re.compile(r"Elapsed:\s*([\d\.]+)\s*s\s*\|\s*Peak RSS:\s*([\d\.]+)\s*MB")


def compute_sha256(engine_bin, table_args, sql):
    cmd = [engine_bin] + table_args + ["--sql", sql]
    h = hashlib.sha256()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    while True:
        chunk = p.stdout.read(65536)
        if not chunk:
            break
        h.update(chunk)
    p.wait()
    if p.returncode != 0:
        err = p.stderr.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Engine failed (code {p.returncode}): {err}")
    return h.hexdigest()


def run_single_iteration(time_cmd_script, engine_bin, table_args, sql, use_no_output):
    cmd = [sys.executable, time_cmd_script, engine_bin] + table_args + ["--sql", sql]
    if use_no_output:
        cmd.append("--no-output")
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    else:
        # Stdout to devnull
        with open(os.devnull, "wb") as devnull:
            proc = subprocess.run(cmd, stdout=devnull, stderr=subprocess.PIPE, text=True)

    if proc.returncode != 0:
        raise RuntimeError(f"Command failed (exit {proc.returncode}): {proc.stderr}")

    m = TIME_CMD_RE.search(proc.stderr)
    if not m:
        raise RuntimeError(f"Could not parse time_cmd output: {proc.stderr}")
    elapsed = float(m.group(1))
    rss = float(m.group(2))
    return elapsed, rss


def main():
    parser = argparse.ArgumentParser(description="Phase 8 Benchmark Runner")
    parser.add_argument("--engine", default="./engine", help="Path to engine executable")
    parser.add_argument("--time-cmd", default="tools/time_cmd.py", help="Path to time_cmd.py")
    parser.add_argument("--single-csv", required=True, help="Path to 5M row single table t.csv")
    parser.add_argument("--join-dir", required=True, help="Directory containing orders.csv, customers.csv, products.csv")
    parser.add_argument("--runs", type=int, default=5, help="Number of runs per query (default: 5)")
    parser.add_argument("--out", default="bench/baseline.tsv", help="Path to output TSV file")
    parser.add_argument("--compare", default=None, help="Path to baseline TSV file to compare against")
    args = parser.parse_args()

    orders_csv = os.path.join(args.join_dir, "orders.csv")
    customers_csv = os.path.join(args.join_dir, "customers.csv")
    products_csv = os.path.join(args.join_dir, "products.csv")

    for fpath in [args.single_csv, orders_csv, customers_csv, products_csv]:
        if not os.path.isfile(fpath):
            print(f"Error: dataset file not found: {fpath}", file=sys.stderr)
            sys.exit(1)

    baseline_data = {}
    if args.compare and os.path.isfile(args.compare):
        with open(args.compare, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split("\t")
                if len(parts) >= 7 and parts[0] != "query":
                    baseline_data[parts[0]] = {
                        "desc": parts[1],
                        "min_s": float(parts[2]),
                        "median_s": float(parts[3]),
                        "max_s": float(parts[4]),
                        "peak_rss_mb": float(parts[5]),
                        "sha256": parts[6]
                    }

    all_queries = []
    single_tables = ["--table", f"t={args.single_csv}"]
    for q_id, sql, no_out, desc in QUERIES_SINGLE:
        all_queries.append((q_id, desc, sql, single_tables, no_out))

    join_tables = [
        "--table", f"orders={orders_csv}",
        "--table", f"customers={customers_csv}",
        "--table", f"products={products_csv}",
    ]
    for q_id, sql, no_out, desc in QUERIES_JOIN:
        all_queries.append((q_id, desc, sql, join_tables, no_out))

    results = []

    print(f"{'Query':<6} | {'Description':<26} | {'Min (s)':<8} | {'Median (s)':<10} | {'Max (s)':<8} | {'Peak RSS':<10} | {'SHA256 (first 8)':<16}")
    print("-" * 98)

    for q_id, desc, sql, table_args, no_out in all_queries:
        # 1. Compute full SHA256 once
        sha = compute_sha256(args.engine, table_args, sql)

        # 2. Run 5 times (run 1 warms OS cache, all 5 recorded)
        times = []
        rss_list = []
        for r in range(args.runs):
            el, rss = run_single_iteration(args.time_cmd, args.engine, table_args, sql, no_out)
            times.append(el)
            rss_list.append(rss)

        sorted_times = sorted(times)
        min_s = sorted_times[0]
        median_s = sorted_times[len(sorted_times) // 2]
        max_s = sorted_times[-1]
        peak_rss = max(rss_list)

        results.append({
            "query": q_id,
            "desc": desc,
            "min_s": min_s,
            "median_s": median_s,
            "max_s": max_s,
            "peak_rss_mb": peak_rss,
            "sha256": sha
        })

        if baseline_data and q_id in baseline_data:
            base_med = baseline_data[q_id]["median_s"]
            base_sha = baseline_data[q_id]["sha256"]
            diff_pct = (median_s - base_med) / base_med * 100.0
            sha_match = "MATCH" if sha == base_sha else "MISMATCH"
            print(f"{q_id:<6} | {desc:<26} | {min_s:6.3f} s | {median_s:6.3f} s ({diff_pct:+5.1f}%) | {max_s:6.3f} s | {peak_rss:7.2f} MB | {sha[:8]} [{sha_match}]")
        else:
            print(f"{q_id:<6} | {desc:<26} | {min_s:6.3f} s | {median_s:6.3f} s | {max_s:6.3f} s | {peak_rss:7.2f} MB | {sha[:8]}")

    # Write TSV
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("query\tdescription\tmin_s\tmedian_s\tmax_s\tpeak_rss_mb\tsha256\n")
        for r in results:
            f.write(f"{r['query']}\t{r['desc']}\t{r['min_s']:.3f}\t{r['median_s']:.3f}\t{r['max_s']:.3f}\t{r['peak_rss_mb']:.2f}\t{r['sha256']}\n")

    print(f"\nSaved benchmark results to {args.out}")


if __name__ == "__main__":
    main()


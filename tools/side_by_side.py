#!/usr/bin/env python3
"""
side_by_side.py - Side-by-side query execution and benchmark comparison interface.
Compares Query Engine vs SQLite, or Baseline vs Current Optimized Engine.
"""

import argparse
import csv
import io
import json
import os
import re
import sqlite3
import subprocess
import sys
import time

from compare import load_csv_into_sqlite, cells_match, rows_match, make_row_sort_key


def run_engine(engine_bin, table_map, sql):
    cmd = [engine_bin]
    for tname, tpath in table_map.items():
        cmd.extend(["--table", f"{tname}={tpath}"])
    cmd.extend(["--sql", sql])

    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    t1 = time.perf_counter()
    elapsed_ms = (t1 - t0) * 1000.0

    if p.returncode != 0:
        return {
            "error": p.stderr.strip() or f"Exit code {p.returncode}",
            "elapsed_ms": elapsed_ms,
            "rows": [],
            "header": []
        }

    reader = csv.reader(io.StringIO(p.stdout))
    all_rows = list(reader)
    if not all_rows:
        return {"header": [], "rows": [], "elapsed_ms": elapsed_ms, "error": None}
    header = all_rows[0]
    data_rows = all_rows[1:]
    return {
        "header": header,
        "rows": data_rows,
        "elapsed_ms": elapsed_ms,
        "error": None
    }


def run_sqlite(conn, sql):
    t0 = time.perf_counter()
    try:
        cur = conn.cursor()
        cur.execute(sql)
        sqlite_rows = cur.fetchall()
        t1 = time.perf_counter()
        elapsed_ms = (t1 - t0) * 1000.0
        header = [d[0] for d in cur.description] if cur.description else []
        return {
            "header": header,
            "rows": sqlite_rows,
            "elapsed_ms": elapsed_ms,
            "error": None
        }
    except Exception as e:
        t1 = time.perf_counter()
        return {
            "header": [],
            "rows": [],
            "elapsed_ms": (t1 - t0) * 1000.0,
            "error": str(e)
        }


def format_side_by_side(eng_res, sq_res, max_display_rows=20):
    lines = []
    lines.append("=" * 100)
    lines.append(f"{'QUERY ENGINE':<48} | {'SQLITE ORACLE':<48}")
    lines.append("-" * 100)

    e_time = f"Time: {eng_res['elapsed_ms']:.2f} ms ({len(eng_res['rows'])} rows)"
    s_time = f"Time: {sq_res['elapsed_ms']:.2f} ms ({len(sq_res['rows'])} rows)"
    lines.append(f"{e_time:<48} | {s_time:<48}")

    if eng_res["error"]:
        lines.append(f"ERROR: {eng_res['error']:<41} | ")
        return "\n".join(lines)
    if sq_res["error"]:
        lines.append(f"{'':<48} | ERROR: {sq_res['error']}")
        return "\n".join(lines)

    speedup = sq_res['elapsed_ms'] / max(eng_res['elapsed_ms'], 0.001)
    status = f"Speedup: {speedup:.2f}x vs SQLite"
    lines.append(f"{status:<48} |")
    lines.append("-" * 100)

    # Check match
    e_rows = eng_res["rows"]
    s_rows = sq_res["rows"]
    same_count = len(e_rows) == len(s_rows)

    is_ordered = bool(re.search(r"\border\s+by\b", sql, re.IGNORECASE)) if "sql" in locals() else True
    e_comp = e_rows
    s_comp = s_rows
    if not is_ordered and same_count and len(e_rows) > 0:
        e_comp = sorted(e_rows, key=make_row_sort_key)
        s_comp = sorted(s_rows, key=make_row_sort_key)

    all_match = same_count and all(rows_match(e, s) for e, s in zip(e_comp, s_comp))
    match_str = "[MATCH: EXACT]" if all_match else "[MISMATCH]"
    lines.append(f"Result Verification: {match_str}")
    lines.append("-" * 100)

    # Header
    e_hdr = ",".join(eng_res["header"])
    s_hdr = ",".join(sq_res["header"])
    lines.append(f"{e_hdr[:48]:<48} | {s_hdr[:48]:<48}")
    lines.append("-" * 100)

    # Rows
    display_count = min(max(len(e_rows), len(s_rows)), max_display_rows)
    for i in range(display_count):
        e_row_str = ",".join(e_rows[i]) if i < len(e_rows) else "<no row>"
        s_row_str = ",".join(str(v) if v is not None else "" for v in s_rows[i]) if i < len(s_rows) else "<no row>"
        m = " " if (i < len(e_rows) and i < len(s_rows) and rows_match(e_rows[i], s_rows[i])) else "!"
        lines.append(f"{e_row_str[:47]:<47}{m}| {s_row_str[:48]:<48}")

    if len(e_rows) > max_display_rows or len(s_rows) > max_display_rows:
        rem_e = max(0, len(e_rows) - max_display_rows)
        rem_s = max(0, len(s_rows) - max_display_rows)
        lines.append(f"... ({rem_e} more rows){'':<29} | ... ({rem_s} more rows)")

    lines.append("=" * 100)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Side-by-Side SQL Query Runner")
    parser.add_argument("--engine", default="./engine", help="Path to engine")
    parser.add_argument("--table", action="append", default=[], help="Table name=file.csv")
    parser.add_argument("--sql", default="", help="SQL query to execute")
    parser.add_argument("--max-rows", type=int, default=20, help="Max preview rows")
    args = parser.parse_args()

    table_map = {}
    for t in args.table:
        parts = t.split("=", 1)
        if len(parts) == 2:
            table_map[parts[0]] = parts[1]

    if not table_map or not args.sql:
        print("Usage: python3 tools/side_by_side.py --table t=data.csv --sql \"SELECT ...\"")
        sys.exit(1)

    # Initialize SQLite memory database
    conn = sqlite3.connect(":memory:")
    for tname, tpath in table_map.items():
        load_csv_into_sqlite(tpath, conn, tname)

    eng_res = run_engine(args.engine, table_map, args.sql)
    sq_res = run_sqlite(conn, args.sql)

    output = format_side_by_side(eng_res, sq_res, args.max_rows)
    print(output)
    conn.close()


if __name__ == "__main__":
    main()


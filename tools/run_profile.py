#!/usr/bin/env python3
"""
run_profile.py - Runs gprof on benchmark queries S1, S3, S4, S7, S10, J1, J5
and prints the top 10 functions by self time with call counts and percentages.
"""

import os
import re
import subprocess
import sys

ENGINE_PROFILE = "./engine_profile"
GMON_OUT = "gmon.out"
SINGLE_CSV = "/tmp/prof_data/t.csv"
JOIN_DIR = "/tmp/prof_data/j"
ORDERS_CSV = os.path.join(JOIN_DIR, "orders.csv")
CUSTOMERS_CSV = os.path.join(JOIN_DIR, "customers.csv")
PRODUCTS_CSV = os.path.join(JOIN_DIR, "products.csv")

TARGET_QUERIES = [
    ("S1", "SELECT COUNT(*) FROM t", ["--table", f"t={SINGLE_CSV}"], True, "scan floor"),
    ("S3", "SELECT SUM(amount), AVG(qty), MIN(id), MAX(id) FROM t", ["--table", f"t={SINGLE_CSV}"], True, "numeric parse + agg"),
    ("S4", "SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city", ["--table", f"t={SINGLE_CSV}"], True, "string-key group"),
    ("S7", "SELECT id, amount FROM t ORDER BY amount DESC, id", ["--table", f"t={SINGLE_CSV}"], True, "full sort, output"),
    ("S10", "SELECT id, city, amount FROM t", ["--table", f"t={SINGLE_CSV}"], False, "output formatting"),
    ("J1", "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id",
     ["--table", f"orders={ORDERS_CSV}", "--table", f"customers={CUSTOMERS_CSV}"], True, "inner join count"),
    ("J5", "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id",
     ["--table", f"orders={ORDERS_CSV}", "--table", f"customers={CUSTOMERS_CSV}"], False, "join output to devnull"),
]


def demangle(name):
    try:
        res = subprocess.run(["c++filt", name], stdout=subprocess.PIPE, text=True)
        return res.stdout.strip()
    except Exception:
        return name


def profile_query(q_id, desc, sql, table_args, use_no_output):
    if os.path.exists(GMON_OUT):
        os.remove(GMON_OUT)

    cmd = [ENGINE_PROFILE] + table_args + ["--sql", sql]
    if use_no_output:
        cmd.append("--no-output")
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    else:
        with open(os.devnull, "wb") as devnull:
            proc = subprocess.run(cmd, stdout=devnull, stderr=subprocess.PIPE, text=True)

    if proc.returncode != 0:
        print(f"Error running {q_id}: {proc.stderr}", file=sys.stderr)
        return

    # Run gprof
    gprof_proc = subprocess.run(["gprof", "-b", "-p", ENGINE_PROFILE, GMON_OUT],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = gprof_proc.stdout.splitlines()

    # Parse flat profile table
    # Columns: % time, cumulative seconds, self seconds, calls, self ms/call, total ms/call, name
    top_funcs = []
    in_table = False
    for line in lines:
        if "time   seconds   seconds" in line:
            in_table = True
            continue
        if not in_table:
            continue
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 6)
        if len(parts) >= 7:
            try:
                pct = float(parts[0])
                cum_s = float(parts[1])
                self_s = float(parts[2])
                calls = parts[3]
                self_ms = parts[4]
                tot_ms = parts[5]
                name = parts[6]
                demangled = demangle(name)
                # Shorten long std types for readability
                short_name = demangled
                for rep in ["std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >",
                            "std::basic_string_view<char, std::char_traits<char> >"]:
                    short_name = short_name.replace(rep, "std::string" if "basic_string<" in rep else "std::string_view")
                top_funcs.append((pct, self_s, calls, short_name))
            except ValueError:
                continue

    print(f"\n================================================================================")
    print(f"Profile for {q_id}: {desc}")
    print(f"SQL: {sql}")
    print(f"{'% Time':<8} | {'Self (s)':<10} | {'Calls':<12} | Function")
    print("-" * 80)
    for pct, self_s, calls, fname in top_funcs[:10]:
        print(f"{pct:6.2f}%  | {self_s:8.2f} s | {calls:<12} | {fname[:65]}")

    if os.path.exists(GMON_OUT):
        os.remove(GMON_OUT)


def main():
    if not os.path.isfile(ENGINE_PROFILE):
        print(f"Building {ENGINE_PROFILE}...", file=sys.stderr)
        subprocess.run(["make", "profile"], check=True)

    for q_id, sql, table_args, no_out, desc in TARGET_QUERIES:
        profile_query(q_id, desc, sql, table_args, no_out)


if __name__ == "__main__":
    main()


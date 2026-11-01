#!/usr/bin/env python3
"""
compare.py - SQL query test comparison runner.
Compares engine output against SQLite.

Rule: Empty CSV fields become SQL NULL everywhere in this project.
SQLite columns are typed as INTEGER, REAL, or TEXT according to inferred types.
The query engine must follow the exact same rule.
"""

import argparse
import csv
import io
import json
import math
import os
import sqlite3
import subprocess
import sys


def infer_column_types(csv_path):
    """
    Infer column types following Phase 1 rules:
    - First line is header.
    - Type inference from first 10,000 data rows:
      INT if every non-empty value is an integer.
      FLOAT if every non-empty value is a number and at least one is not an integer.
      Else STRING.
      All-empty column is STRING.
    """
    with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            return []
            
        num_cols = len(header)
        all_empty = [True] * num_cols
        all_int = [True] * num_cols
        all_num = [True] * num_cols
        has_non_int_num = [False] * num_cols
        
        row_count = 0
        for row in reader:
            row_count += 1
            if row_count > 10000:
                break
            for idx in range(num_cols):
                val = row[idx].strip() if idx < len(row) else ""
                if val == "":
                    continue
                all_empty[idx] = False
                
                # Check integer
                is_i = False
                try:
                    int(val)
                    is_i = True
                except ValueError:
                    is_i = False
                    
                if not is_i:
                    all_int[idx] = False
                    
                # Check float
                is_f = False
                val_clean = val.lower().lstrip("+-")
                if not val_clean.startswith(("nan", "inf", "0x")):
                    try:
                        float(val)
                        is_f = True
                    except ValueError:
                        is_f = False
                    
                if is_f:
                    if not is_i:
                        has_non_int_num[idx] = True
                else:
                    all_num[idx] = False
                    
        types = []
        for idx in range(num_cols):
            if all_empty[idx]:
                types.append("TEXT")
            elif all_int[idx]:
                types.append("INTEGER")
            elif all_num[idx] and has_non_int_num[idx]:
                types.append("REAL")
            else:
                types.append("TEXT")
                
        return list(zip(header, types))


def load_csv_into_sqlite(csv_path, conn, table_name="t"):
    """
    Load CSV into SQLite table `table_name` with typed columns.
    Empty CSV field means SQL NULL.
    """
    col_defs = infer_column_types(csv_path)
    if not col_defs:
        return
        
    create_cols = [f'"{name}" {col_type}' for name, col_type in col_defs]
    sql_create = f'CREATE TABLE "{table_name}" ({", ".join(create_cols)})'
    conn.execute(sql_create)
    
    placeholders = ", ".join(["?"] * len(col_defs))
    sql_insert = f'INSERT INTO "{table_name}" VALUES ({placeholders})'
    
    with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        next(reader, None)  # Skip header
        
        batch = []
        for row in reader:
            typed_row = []
            for idx, (_, col_type) in enumerate(col_defs):
                val = row[idx].strip() if idx < len(row) else ""
                if val == "":
                    typed_row.append(None)
                elif col_type == "INTEGER":
                    try:
                        typed_row.append(int(val))
                    except ValueError:
                        typed_row.append(val)
                elif col_type == "REAL":
                    try:
                        fval = float(val)
                        if math.isnan(fval) or math.isinf(fval):
                            typed_row.append(val)
                        else:
                            typed_row.append(fval)
                    except ValueError:
                        typed_row.append(val)
                else:
                    typed_row.append(row[idx] if idx < len(row) else "")
            batch.append(typed_row)
            if len(batch) >= 10000:
                conn.executemany(sql_insert, batch)
                batch = []
                
        if batch:
            conn.executemany(sql_insert, batch)
    conn.commit()


def make_cell_sort_key(val):
    """
    NULL first: (0, 0, '')
    Number:     (1, float_val, '')
    String:     (2, 0, str_val)
    """
    if val is None or val == "":
        return (0, 0, "")
    s = str(val)
    s_clean = s.lower().lstrip("+-")
    if s_clean.startswith(("nan", "inf")):
        return (2, 0, s)
    try:
        f = float(val)
        return (1, f, "")
    except (ValueError, TypeError):
        return (2, 0, s)


def make_row_sort_key(row):
    return tuple(make_cell_sort_key(cell) for cell in row)


def cells_match(engine_cell, sqlite_cell):
    """
    Compare engine string cell with SQLite value.
    Empty string in engine stdout represents SQL NULL.
    Parse each engine cell as int if it matches integer pattern, float otherwise (or string).
    Compare numerically with tolerance when either side is a float.
    Treat SQLite 5.0 and engine "5.0" or 5 and "5.0" as equal numerically.
    """
    if sqlite_cell is None:
        return engine_cell == ""
    if engine_cell == "":
        return sqlite_cell is None

    eng_is_int = False
    eng_int = 0
    eng_is_float = False
    eng_float = 0.0

    try:
        eng_int = int(engine_cell)
        eng_is_int = True
    except ValueError:
        val_clean = str(engine_cell).lower().lstrip("+-")
        if not val_clean.startswith(("nan", "inf")):
            try:
                eng_float = float(engine_cell)
                eng_is_float = True
            except ValueError:
                pass

    sq_is_num = isinstance(sqlite_cell, (int, float))
    eng_is_num = eng_is_int or eng_is_float

    if eng_is_num and sq_is_num:
        # Either side is float -> compare with float tolerance
        if eng_is_float or isinstance(sqlite_cell, float):
            val_eng = float(eng_int) if eng_is_int else eng_float
            val_sq = float(sqlite_cell)
            return math.isclose(val_eng, val_sq, rel_tol=1e-6, abs_tol=1e-6)
        else:
            return eng_int == sqlite_cell

    return str(engine_cell) == str(sqlite_cell)


def rows_match(engine_row, sqlite_row):
    if len(engine_row) != len(sqlite_row):
        return False
    for ec, sc in zip(engine_row, sqlite_row):
        if not cells_match(ec, sc):
            return False
    return True


def run_test(query_info, table_map, engine_bin, conn):
    q_name = query_info.get("name", "unnamed")
    sql = query_info.get("sql", "")
    raw_args = query_info.get("engine_args")
    if not raw_args:
        if len(table_map) == 1 and "t" in table_map:
            raw_args = ["--table", "t={CSV}", "--sql", sql]
        else:
            raw_args = []
            for tname in table_map:
                raw_args.extend(["--table", f"{tname}={{{tname}}}"])
            raw_args.extend(["--sql", sql])
    ordered = query_info.get("ordered", False)
    
    # Run SQLite
    try:
        cur = conn.cursor()
        cur.execute(sql)
        sqlite_rows = cur.fetchall()
    except Exception as e:
        print(f"[FAIL] {q_name}: SQLite execution error: {e}")
        return False

    # Run Engine
    engine_cmd = [engine_bin]
    for arg in raw_args:
        val = arg
        if "{CSV}" in val and "t" in table_map:
            val = val.replace("{CSV}", table_map["t"])
        for tname, tpath in table_map.items():
            val = val.replace("{" + tname + "}", tpath)
        engine_cmd.append(val)
    try:
        proc = subprocess.run(
            engine_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30
        )
    except Exception as e:
        print(f"[FAIL] {q_name}: Engine process launch error: {e}")
        return False
        
    if proc.returncode != 0:
        print(f"[FAIL] {q_name}: Engine exited with code {proc.returncode}")
        print(f"  SQL: {sql}")
        if proc.stderr:
            print(f"  Stderr: {proc.stderr.strip()}")
        return False

    # Parse Engine CSV output
    reader = csv.reader(io.StringIO(proc.stdout))
    try:
        engine_rows_all = list(reader)
    except Exception as e:
        print(f"[FAIL] {q_name}: Failed to parse engine stdout as CSV: {e}")
        return False
        
    if not engine_rows_all:
        engine_header = []
        engine_rows = []
    else:
        engine_header = engine_rows_all[0]
        num_cols = len(engine_header)
        engine_rows = []
        for r in engine_rows_all[1:]:
            # Python csv.reader returns [] for empty lines. For a single column, an empty line represents [''] (SQL NULL)
            if len(r) == 0 and num_cols == 1:
                engine_rows.append([""])
            else:
                engine_rows.append(r)
        
    # Check column counts (ignores header names but checks column count)
    sq_num_cols = len(cur.description) if cur.description else 0
    eng_num_cols = len(engine_header)
    if sq_num_cols != eng_num_cols:
        print(f"[FAIL] {q_name}: Column count mismatch: SQLite={sq_num_cols}, Engine={eng_num_cols}")
        return False

    # Check row counts
    sq_count = len(sqlite_rows)
    eng_count = len(engine_rows)
    
    # Sort if unordered
    if not ordered:
        engine_rows = sorted(engine_rows, key=make_row_sort_key)
        sqlite_rows = sorted(sqlite_rows, key=make_row_sort_key)
        
    # Compare rows
    differing_eng = []
    differing_sq = []
    max_len = max(sq_count, eng_count)
    
    for i in range(max_len):
        e_row = engine_rows[i] if i < eng_count else ["<MISSING ROW>"]
        s_row = sqlite_rows[i] if i < sq_count else ["<MISSING ROW>"]
        
        if i >= eng_count or i >= sq_count or not rows_match(e_row, s_row):
            if len(differing_eng) < 5:
                differing_eng.append(e_row)
                differing_sq.append(s_row)
                
    if differing_eng or sq_count != eng_count:
        print(f"[FAIL] {q_name}")
        print(f"  SQL: {sql}")
        print(f"  Row count: SQLite={sq_count}, Engine={eng_count}")
        print("  First differing rows:")
        for idx, (e_r, s_r) in enumerate(zip(differing_eng, differing_sq)):
            print(f"    Diff #{idx + 1}:")
            print(f"      SQLite: {s_r}")
            print(f"      Engine: {e_r}")
        return False
        
    print(f"[PASS] {q_name}")
    return True


def main():
    parser = argparse.ArgumentParser(description="Compare query engine output against SQLite.")
    parser.add_argument("--csv", required=False, default=None, help="Input CSV path")
    parser.add_argument("--table", action="append", default=[], help="Table definition name=path")
    parser.add_argument("--engine", required=True, help="Path to engine executable")
    parser.add_argument("--queries", required=True, help="Path to queries.json")
    args = parser.parse_args()
    
    table_map = {}
    if args.table:
        for t_spec in args.table:
            parts = t_spec.split("=", 1)
            if len(parts) == 2:
                table_map[parts[0]] = parts[1]
    if args.csv:
        table_map["t"] = args.csv
        
    if not table_map:
        print("Error: either --csv or at least one --table must be provided.", file=sys.stderr)
        sys.exit(1)
        
    for tname, tpath in table_map.items():
        if not os.path.isfile(tpath):
            print(f"Error: CSV file '{tpath}' not found.", file=sys.stderr)
            sys.exit(1)
        
    if not os.path.isfile(args.queries):
        print(f"Error: queries file '{args.queries}' not found.", file=sys.stderr)
        sys.exit(1)
        
    with open(args.queries, "r", encoding="utf-8") as f:
        queries = json.load(f)
        
    conn = sqlite3.connect(":memory:")
    for tname, tpath in table_map.items():
        load_csv_into_sqlite(tpath, conn, tname)
    
    all_passed = True
    for q in queries:
        ok = run_test(q, table_map, args.engine, conn)
        if not ok:
            all_passed = False
            
    conn.close()
    if not all_passed:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()

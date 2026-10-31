#!/usr/bin/env python3
"""
run_parser_tests.py - Runs all positive, negative, and round-trip tests.
"""

import glob
import os
import subprocess
import sys

PARSER_DIR = "tests/parser"
SQLPARSE_BIN = "./sqlparse"


def test_positive():
    pos_files = sorted(glob.glob(os.path.join(PARSER_DIR, "*.ast")))
    if not pos_files:
        print("[FAIL] No positive test cases found!")
        return False
        
    print(f"Running {len(pos_files)} positive and round-trip parser tests...")
    all_ok = True
    
    for ast_file in pos_files:
        base = ast_file[:-4]
        sql_file = base + ".sql"
        case_name = os.path.basename(base)
        
        with open(ast_file, "r", encoding="utf-8") as f:
            expected_ast = f.read().strip()
            
        # 1. Test AST generation
        proc = subprocess.run([SQLPARSE_BIN, "-f", sql_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if proc.returncode != 0:
            print(f"[FAIL] {case_name}: exited with code {proc.returncode}")
            print(f"  stderr: {proc.stderr.strip()}")
            all_ok = False
            continue
            
        actual_ast = proc.stdout.strip()
        if actual_ast != expected_ast:
            print(f"[FAIL] {case_name}: AST mismatch")
            print(f"  Expected:\n{expected_ast}")
            print(f"  Got:\n{actual_ast}")
            all_ok = False
            continue
            
        # 2. Round-trip test: SQL -> parse -> toSql -> parse -> AST == original AST
        rt_proc = subprocess.run([SQLPARSE_BIN, "--roundtrip", "-f", sql_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if rt_proc.returncode != 0:
            print(f"[FAIL] {case_name}: round-trip failure")
            print(f"  stderr: {rt_proc.stderr.strip()}")
            all_ok = False
            continue
            
        rt_ast = rt_proc.stdout.strip()
        if rt_ast != expected_ast:
            print(f"[FAIL] {case_name}: round-trip AST mismatch")
            all_ok = False
            continue
            
    if all_ok:
        print(f"[OK] All {len(pos_files)} positive and round-trip tests PASSED.")
    return all_ok


def test_negative():
    err_files = sorted(glob.glob(os.path.join(PARSER_DIR, "*.err")))
    if not err_files:
        print("[FAIL] No negative test cases found!")
        return False
        
    print(f"Running {len(err_files)} negative parser tests...")
    all_ok = True
    
    for err_file in err_files:
        base = err_file[:-4]
        sql_file = base + ".sql"
        case_name = os.path.basename(base)
        
        with open(err_file, "r", encoding="utf-8") as f:
            expected_err_substring = f.read().strip()
            
        proc = subprocess.run([SQLPARSE_BIN, "-f", sql_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if proc.returncode != 1:
            print(f"[FAIL] {case_name}: expected exit code 1, got {proc.returncode}")
            all_ok = False
            continue
            
        if expected_err_substring not in proc.stderr:
            print(f"[FAIL] {case_name}: expected substring '{expected_err_substring}' not in stderr")
            print(f"  stderr: {proc.stderr.strip()}")
            all_ok = False
            continue
            
    if all_ok:
        print(f"[OK] All {len(err_files)} negative tests PASSED.")
    return all_ok


import tempfile


def test_deep_nesting_100k():
    print("Testing recursion depth limit with 100,000 '(' characters...")
    # Feed 100,000 '(' characters to ensure parser raises ParseError without crashing or stack overflow
    deep_sql = "SELECT " + "(" * 100000 + "1" + ")" * 100000 + " FROM t;"
    with tempfile.NamedTemporaryFile("w", delete=False) as tf:
        tf.write(deep_sql)
        temp_path = tf.name

    try:
        proc = subprocess.run([SQLPARSE_BIN, "-f", temp_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    finally:
        if os.path.exists(temp_path):
            os.remove(temp_path)

    if proc.returncode != 1:
        print(f"[FAIL] 100k deep nesting: expected exit code 1, got {proc.returncode}")
        return False
    if "exceeded maximum nesting depth of 200" not in proc.stderr:
        print(f"[FAIL] 100k deep nesting: expected depth error not in stderr: {proc.stderr}")
        return False
    print("[OK] 100,000 '(' recursion depth limit PASSED without crash or stack overflow.")
    return True


def main():
    if not os.path.isfile(SQLPARSE_BIN):
        subprocess.run(["make", "sqlparse"], check=True)
        
    ok1 = test_positive()
    ok2 = test_negative()
    ok3 = test_deep_nesting_100k()
    
    if ok1 and ok2 and ok3:
        print("=== All Parser Tests PASSED! ===")
        sys.exit(0)
    else:
        print("=== Some Parser Tests FAILED! ===")
        sys.exit(1)


if __name__ == "__main__":
    main()

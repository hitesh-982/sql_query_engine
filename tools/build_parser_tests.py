#!/usr/bin/env python3
"""
build_parser_tests.py - Generates comprehensive positive and negative test cases for Phase 4.
"""

import os
import subprocess
import sys

PARSER_DIR = "tests/parser"
os.makedirs(PARSER_DIR, exist_ok=True)

# 45 Positive Cases
POSITIVE_CASES = [
    # 1. Minimal select
    ("01_select_star", "SELECT * FROM t;"),
    # 2. Select columns
    ("02_select_cols", "SELECT a, b, c FROM t;"),
    # 3. DISTINCT
    ("03_distinct", "SELECT DISTINCT a, b FROM t;"),
    # 4. Table qualification
    ("04_qualified_cols", "SELECT t.a, t.b FROM t;"),
    # 5. Wildcard table.*
    ("05_table_star", "SELECT t.* FROM t;"),
    # 6. Aliases with AS
    ("06_alias_as", "SELECT a AS col_a, b AS col_b FROM t AS my_table;"),
    # 7. Implicit aliases
    ("07_alias_implicit", "SELECT a col_a, b col_b FROM t my_table;"),
    # 8. Unary minus and NOT
    ("08_unary_ops", "SELECT -a, NOT b FROM t;"),
    # 9. Binary arithmetic precedence: + - * /
    ("09_arithmetic_precedence", "SELECT 1 + 2 * 3 - 4 / 2 FROM t;"),
    # 10. Nested parentheses
    ("10_nested_parens", "SELECT ((1 + 2) * (3 - 4)) FROM t;"),
    # 11. Comparison operators
    ("11_comparisons", "SELECT a = b, c != d, e < f, g <= h, i > j, k >= l FROM t;"),
    # 12. <> operator (normalized to != in AST)
    ("12_diamond_comparison", "SELECT a <> b FROM t;"),
    # 13. Boolean AND / OR precedence
    ("13_and_or_precedence", "SELECT a OR b AND c FROM t;"),
    # 14. NOT with comparison
    ("14_not_precedence", "SELECT NOT a = b FROM t;"),
    # 15. IS NULL
    ("15_is_null", "SELECT a FROM t WHERE a IS NULL;"),
    # 16. IS NOT NULL
    ("16_is_not_null", "SELECT a FROM t WHERE a IS NOT NULL;"),
    # 17. BETWEEN alone
    ("17_between", "SELECT a FROM t WHERE a BETWEEN 10 AND 20;"),
    # 18. NOT BETWEEN
    ("18_not_between", "SELECT a FROM t WHERE a NOT BETWEEN 10 AND 20;"),
    # 19. BETWEEN inside larger AND
    ("19_between_inside_and", "SELECT a FROM t WHERE x AND y BETWEEN 1 AND 10 AND z;"),
    # 20. IN list of integers
    ("20_in_list_ints", "SELECT a FROM t WHERE a IN (1, 2, 3);"),
    # 21. NOT IN list
    ("21_not_in_list", "SELECT a FROM t WHERE a NOT IN ('x', 'y');"),
    # 22. LIKE pattern
    ("22_like", "SELECT a FROM t WHERE name LIKE '%abc%';"),
    # 23. NOT LIKE pattern
    ("23_not_like", "SELECT a FROM t WHERE name NOT LIKE 'test_';"),
    # 24. INNER JOIN with ON
    ("24_inner_join", "SELECT * FROM t INNER JOIN u ON t.id = u.id;"),
    # 25. Plain JOIN with ON
    ("25_plain_join", "SELECT * FROM t JOIN u ON t.id = u.id;"),
    # 26. LEFT JOIN with ON
    ("26_left_join", "SELECT * FROM t LEFT JOIN u ON t.id = u.id;"),
    # 27. LEFT OUTER JOIN with ON
    ("27_left_outer_join", "SELECT * FROM t LEFT OUTER JOIN u ON t.id = u.id;"),
    # 28. RIGHT JOIN with ON
    ("28_right_join", "SELECT * FROM t RIGHT JOIN u ON t.id = u.id;"),
    # 29. RIGHT OUTER JOIN with ON
    ("29_right_outer_join", "SELECT * FROM t RIGHT OUTER JOIN u ON t.id = u.id;"),
    # 30. FULL JOIN with ON
    ("30_full_join", "SELECT * FROM t FULL JOIN u ON t.id = u.id;"),
    # 31. FULL OUTER JOIN with ON
    ("31_full_outer_join", "SELECT * FROM t FULL OUTER JOIN u ON t.id = u.id;"),
    # 32. Multiple JOINs
    ("32_multiple_joins", "SELECT * FROM a JOIN b ON a.id = b.id LEFT JOIN c ON b.id = c.id;"),
    # 33. WHERE clause
    ("33_where_clause", "SELECT a FROM t WHERE a > 10;"),
    # 34. GROUP BY clause
    ("34_group_by", "SELECT a, b FROM t GROUP BY a, b;"),
    # 35. HAVING clause
    ("35_having", "SELECT a, sum(b) FROM t GROUP BY a HAVING sum(b) > 100;"),
    # 36. ORDER BY ASC and DESC
    ("36_order_by", "SELECT a, b FROM t ORDER BY a ASC, b DESC;"),
    # 37. LIMIT alone
    ("37_limit", "SELECT a FROM t LIMIT 10;"),
    # 38. LIMIT with OFFSET
    ("38_limit_offset", "SELECT a FROM t LIMIT 10 OFFSET 5;"),
    # 39. COUNT(*) function
    ("39_count_star", "SELECT count(*) FROM t;"),
    # 40. COUNT(DISTINCT expr)
    ("40_count_distinct", "SELECT count(DISTINCT city) FROM t;"),
    # 41. Other aggregates SUM, AVG, MIN, MAX
    ("41_aggregates", "SELECT sum(amount), avg(qty), min(id), max(id) FROM t;"),
    # 42. COUNT as column identifier (Change 3)
    ("42_count_as_column", "SELECT count, min FROM t;"),
    # 43. Quoted identifiers
    ("43_quoted_identifiers", "SELECT \"my col\", \"another\" FROM \"my table\";"),
    # 44. Line and block comments
    ("44_comments", "-- leading comment\nSELECT a /* inline block */, b FROM t; -- trailing"),
    # 45. Mixed-case keywords and escaped strings
    ("45_mixed_case_escapes", "sElEcT 'It''s fine', NULL FROM t;")
]

# 27 Negative Cases (with expected error substrings)
NEGATIVE_CASES = [
    ("01_missing_from", "SELECT a, b WHERE x > 1;", "expected FROM"),
    ("02_trailing_comma_select", "SELECT a, b, FROM t;", "trailing comma in select list"),
    ("03_unbalanced_paren", "SELECT (1 + 2 FROM t;", "unbalanced parenthesis"),
    ("04_unterminated_string", "SELECT 'unterminated FROM t;", "unterminated string"),
    ("05_unterminated_block_comment", "SELECT 1 /* comment FROM t;", "unterminated block comment"),
    ("06_join_without_on", "SELECT * FROM t JOIN u;", "expected ON"),
    ("07_limit_non_integer", "SELECT * FROM t LIMIT 'five';", "LIMIT requires an integer literal"),
    ("08_limit_negative", "SELECT * FROM t LIMIT -5;", "LIMIT must be a non-negative integer"),
    ("09_empty_input", "", "empty input; expected SELECT"),
    ("10_stray_token_after_stmt", "SELECT * FROM t; SELECT * FROM t;", "unexpected token after statement"),
    ("11_select_from_t", "SELECT FROM t;", "expected select item"),
    ("12_bad_operator", "SELECT @ FROM t;", "unexpected token in expression"),
    ("13_missing_table_after_from", "SELECT * FROM;", "expected table name"),
    ("14_trailing_comma_group_by", "SELECT a FROM t GROUP BY a,;", "trailing comma in GROUP BY list"),
    ("15_trailing_comma_order_by", "SELECT a FROM t ORDER BY a,;", "trailing comma in ORDER BY list"),
    ("16_trailing_comma_in_list", "SELECT * FROM t WHERE a IN (1, 2,);", "trailing comma in IN list"),
    ("17_missing_closing_paren_in", "SELECT * FROM t WHERE a IN (1, 2;", "expected ')' after IN list"),
    ("18_missing_expr_after_and", "SELECT * FROM t WHERE a AND;", "unexpected token in expression"),
    ("19_missing_expr_after_plus", "SELECT * FROM t WHERE a +;", "unexpected token in expression"),
    ("20_missing_on_expr", "SELECT * FROM t JOIN u ON;", "unexpected token in expression"),
    ("21_missing_col_after_dot", "SELECT t. FROM t;", "expected column name or * after '.'"),
    ("22_missing_alias_after_as", "SELECT a AS FROM t;", "expected alias after AS"),
    ("23_missing_by_after_group", "SELECT a FROM t GROUP a;", "expected BY after GROUP"),
    ("24_missing_by_after_order", "SELECT a FROM t ORDER a;", "expected BY after ORDER"),
    ("25_missing_null_after_is", "SELECT a FROM t WHERE a IS;", "expected NULL after IS"),
    ("26_missing_null_after_is_not", "SELECT a FROM t WHERE a IS NOT;", "expected NULL after IS"),
    ("27_deeply_nested_parens", "SELECT " + "(" * 205 + "1" + ")" * 205 + " FROM t;", "exceeded maximum nesting depth of 200")
]


def main():
    # Build sqlparse if needed
    subprocess.run(["make", "sqlparse"], check=True)

    print("Generating Positive Test Cases...")
    for name, sql in POSITIVE_CASES:
        sql_path = os.path.join(PARSER_DIR, f"{name}.sql")
        ast_path = os.path.join(PARSER_DIR, f"{name}.ast")
        
        with open(sql_path, "w", encoding="utf-8") as f:
            f.write(sql + "\n")
            
        res = subprocess.run(["./sqlparse", sql], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            print(f"Error generating AST for {name}: {res.stderr}")
            sys.exit(1)
            
        with open(ast_path, "w", encoding="utf-8") as f:
            f.write(res.stdout)

    print(f"Generated {len(POSITIVE_CASES)} positive test cases.")

    print("Generating Negative Test Cases...")
    for name, sql, expected_err in NEGATIVE_CASES:
        sql_path = os.path.join(PARSER_DIR, f"{name}.sql")
        err_path = os.path.join(PARSER_DIR, f"{name}.err")
        
        with open(sql_path, "w", encoding="utf-8") as f:
            f.write(sql + "\n")
            
        with open(err_path, "w", encoding="utf-8") as f:
            f.write(expected_err + "\n")

    print(f"Generated {len(NEGATIVE_CASES)} negative test cases.")


if __name__ == "__main__":
    main()

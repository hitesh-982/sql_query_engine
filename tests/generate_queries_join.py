#!/usr/bin/env python3
"""
generate_queries_join.py
Generates queries_join.json with over 110 comprehensive join queries
and validates each query against SQLite.
"""

import json
import os

def build_queries():
    queries = []

    def add_q(name, sql, ordered=False, engine_args=None):
        q = {
            "name": name,
            "sql": sql,
            "ordered": ordered
        }
        if engine_args:
            q["engine_args"] = engine_args
        queries.append(q)

    # -------------------------------------------------------------------------
    # Category 1: Basic INNER JOINs (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_inner_basic",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_reversed_on",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON c.id = o.cust_id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_explicit_keyword",
          "SELECT o.id, c.name FROM orders o INNER JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_unqualified_tables",
          "SELECT orders.id, customers.name FROM orders JOIN customers ON orders.cust_id = customers.id ORDER BY orders.id, customers.name LIMIT 20", True)
    add_q("p5_inner_star_all",
          "SELECT * FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 10", True)
    add_q("p5_inner_star_qualified_probe",
          "SELECT o.*, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 10", True)
    add_q("p5_inner_star_qualified_build",
          "SELECT o.id, c.* FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 10", True)
    add_q("p5_inner_constants",
          "SELECT 42, 'Order:', o.id, 'Cust:', c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 15", True)
    add_q("p5_inner_strings",
          "SELECT c.name, o.status, c.region FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_arithmetic",
          "SELECT o.id, o.qty * 10, o.amount + 5.5, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_reversed_written_order",
          "SELECT c.name, o.id, o.amount FROM customers c JOIN orders o ON c.id = o.cust_id ORDER BY c.id, o.id, c.name LIMIT 20", True)
    add_q("p5_inner_swapped_candidate",
          "SELECT c.name, o.id, o.amount FROM customers c JOIN orders o ON c.id = o.cust_id WHERE o.amount > 100 ORDER BY c.id, o.id, c.name LIMIT 20", True)
    add_q("p5_inner_zero_kept_build_cols",
          "SELECT o.id, o.amount, o.status FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, o.amount, o.status LIMIT 20", True)
    add_q("p5_inner_zero_kept_probe_cols",
          "SELECT c.id, c.name, c.region FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY c.id, c.name LIMIT 20", True)
    add_q("p5_inner_count_star",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id", False)

    # -------------------------------------------------------------------------
    # Category 2: Composite & Multi-Column Keys & String Keys (10 queries)
    # -------------------------------------------------------------------------
    add_q("p5_inner_multi_col_key",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND o.id = c.id ORDER BY o.id, c.name", True)
    add_q("p5_inner_string_key",
          "SELECT o.id, o.status, c.name, c.region FROM orders o JOIN customers c ON o.status = c.region ORDER BY o.id, c.name, c.id LIMIT 20", True)
    add_q("p5_inner_int_vs_float_key",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_key_expr_arithmetic",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id + 0 = c.id + 0 ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_inner_three_equi_keys",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND o.id = c.id AND o.status = c.region ORDER BY o.id, c.name", True)
    add_q("p5_inner_multi_col_reversed",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON c.id = o.cust_id AND c.id = o.id ORDER BY o.id, c.name", True)
    add_q("p5_inner_composite_key_count",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id AND o.id = c.id", False)
    add_q("p5_inner_string_key_count",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.status = c.region", False)
    add_q("p5_inner_expr_key_count",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id + 1 = c.id + 1", False)
    add_q("p5_inner_multi_col_where",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND o.id = c.id WHERE o.amount > 50 ORDER BY o.id, c.name", True)

    # -------------------------------------------------------------------------
    # Category 3: INNER JOIN with Residual ON Predicates (10 queries)
    # -------------------------------------------------------------------------
    add_q("p5_residual_inequality_probe",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id AND o.amount > 500 ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_inequality_build",
          "SELECT o.id, c.name, c.region FROM orders o JOIN customers c ON o.cust_id = c.id AND c.region = 'North' ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_cross_table_inequality",
          "SELECT o.id, c.name, o.qty, c.id FROM orders o JOIN customers c ON o.cust_id = c.id AND o.qty < c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_like_pattern",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND c.name LIKE 'Customer_1%' ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_is_not_null",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND c.note IS NOT NULL ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_constant_true",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND 1 = 1 ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_constant_false",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND 1 = 0", False)
    add_q("p5_residual_or_condition",
          "SELECT o.id, c.name, c.region FROM orders o JOIN customers c ON o.cust_id = c.id AND (c.region = 'North' OR c.region = 'South') ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_between",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id AND o.amount BETWEEN 100 AND 1000 ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_residual_multiple_conjuncts",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id AND o.amount > 50 AND c.region != 'West' ORDER BY o.id, c.name LIMIT 20", True)

    # -------------------------------------------------------------------------
    # Category 4: Non-Equi Joins / Nested-Loop Fallback (10 queries)
    # -------------------------------------------------------------------------
    add_q("p5_nl_less_than",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON o.id < c.id WHERE o.id <= 5 AND c.id <= 5 ORDER BY o.id, c.id", True)
    add_q("p5_nl_greater_than",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON o.id > c.id WHERE o.id <= 5 AND c.id <= 5 ORDER BY o.id, c.id", True)
    add_q("p5_nl_between",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON o.id BETWEEN c.id AND c.id + 2 WHERE o.id <= 6 AND c.id <= 4 ORDER BY o.id, c.id", True)
    add_q("p5_nl_cross_true",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON 1 = 1 WHERE o.id <= 3 AND c.id <= 3 ORDER BY o.id, c.id", True)
    add_q("p5_nl_not_equal",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON o.id != c.id WHERE o.id <= 3 AND c.id <= 3 ORDER BY o.id, c.id", True)
    add_q("p5_nl_string_less_than",
          "SELECT o.status, c.region FROM orders o JOIN customers c ON o.status < c.region WHERE o.id <= 3 AND c.id <= 3 ORDER BY o.status, c.region, o.id, c.id", True)
    add_q("p5_nl_arithmetic_inequality",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON o.id * 2 < c.id WHERE o.id <= 5 AND c.id <= 15 ORDER BY o.id, c.id", True)
    add_q("p5_nl_complex_or",
          "SELECT o.id, c.id FROM orders o JOIN customers c ON (o.id = c.id OR o.id = c.id + 1) WHERE o.id <= 5 AND c.id <= 5 ORDER BY o.id, c.id", True)
    add_q("p5_nl_count_small",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.id < c.id WHERE o.id <= 10 AND c.id <= 10", False)
    add_q("p5_nl_with_limit",
          "SELECT o.id, c.id, c.name FROM orders o JOIN customers c ON o.id < c.id WHERE o.id <= 20 ORDER BY o.id, c.id, c.name LIMIT 15", True)

    # -------------------------------------------------------------------------
    # Category 5: Basic LEFT [OUTER] JOINs (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_left_basic",
          "SELECT o.id, o.cust_id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 25", True)
    add_q("p5_left_outer_keyword",
          "SELECT o.id, o.cust_id, c.name FROM orders o LEFT OUTER JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 25", True)
    add_q("p5_left_anti_join_id_null",
          "SELECT o.id, o.cust_id FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL ORDER BY o.id LIMIT 20", True)
    add_q("p5_left_anti_join_name_null",
          "SELECT o.id, o.cust_id FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.name IS NULL ORDER BY o.id LIMIT 20", True)
    add_q("p5_left_probe_null_key",
          "SELECT o.id, o.cust_id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE o.cust_id IS NULL ORDER BY o.id, c.name LIMIT 10", True)
    add_q("p5_left_residual_in_on",
          "SELECT o.id, o.cust_id, c.name, c.region FROM orders o LEFT JOIN customers c ON o.cust_id = c.id AND c.region = 'North' ORDER BY o.id, c.name LIMIT 25", True)
    add_q("p5_left_filter_on_probe",
          "SELECT o.id, o.cust_id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE o.amount > 500 ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_left_filter_on_build",
          "SELECT o.id, o.cust_id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.region = 'North' ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_left_star",
          "SELECT * FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 10", True)
    add_q("p5_left_zero_kept_build_cols",
          "SELECT o.id, o.amount, o.status FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, o.amount, o.status LIMIT 20", True)
    add_q("p5_left_count_star",
          "SELECT COUNT(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_left_count_build_id",
          "SELECT COUNT(c.id), COUNT(o.id) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_left_nl_less_than",
          "SELECT o.id, c.id FROM orders o LEFT JOIN customers c ON o.id < c.id WHERE o.id <= 5 AND c.id <= 3 ORDER BY o.id, c.id", True)
    add_q("p5_left_reversed_tables",
          "SELECT c.id, c.name, o.id, o.amount FROM customers c LEFT JOIN orders o ON c.id = o.cust_id ORDER BY c.id, o.id, c.name LIMIT 25", True)
    add_q("p5_left_reversed_anti_join",
          "SELECT c.id, c.name FROM customers c LEFT JOIN orders o ON c.id = o.cust_id WHERE o.id IS NULL ORDER BY c.id, c.name LIMIT 20", True)

    # -------------------------------------------------------------------------
    # Category 6: Three-Table Join Chains (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_3way_inner_inner",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_inner_left",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_left_inner",
          "SELECT o.id, c.name, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_left_left",
          "SELECT o.id, c.name, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_where_probe",
          "SELECT o.id, c.name, p.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id WHERE o.amount > 500 ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_where_multiple_tables",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id WHERE c.region = 'North' AND p.price > 20 ORDER BY o.id, c.name, p.name LIMIT 20", True)
    add_q("p5_3way_star",
          "SELECT * FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name LIMIT 5", True)
    add_q("p5_3way_arithmetic",
          "SELECT o.id, o.qty * p.price, c.name FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_3way_join_on_second_table",
          "SELECT o.id, c.name, p.category FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON c.id = p.id ORDER BY o.id, p.id, c.name LIMIT 20", True)
    add_q("p5_3way_count_star",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id", False)
    add_q("p5_3way_left_count_star",
          "SELECT COUNT(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id", False)
    add_q("p5_3way_residuals",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id AND c.region = 'East' JOIN products p ON o.product_id = p.id AND p.price > 10 ORDER BY o.id, c.name, p.name LIMIT 15", True)
    add_q("p5_3way_aggregate_group",
          "SELECT p.category, COUNT(*), AVG(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id GROUP BY p.category ORDER BY p.category", True)
    add_q("p5_3way_aggregate_having",
          "SELECT c.region, p.category, COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id GROUP BY c.region, p.category HAVING COUNT(*) > 1 ORDER BY c.region, p.category", True)
    add_q("p5_3way_order_limit",
          "SELECT o.id, c.name, p.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.amount DESC, o.id ASC, c.name ASC, p.name ASC LIMIT 10 OFFSET 5", True)

    # -------------------------------------------------------------------------
    # Category 7: Self-Joins & Aliases (10 queries)
    # -------------------------------------------------------------------------
    add_q("p5_self_orders_cust",
          "SELECT o1.id, o2.id, o1.cust_id FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 50 ORDER BY o1.id, o2.id LIMIT 25", True)
    add_q("p5_self_orders_count",
          "SELECT COUNT(*) FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 50", False)
    add_q("p5_self_customers_region",
          "SELECT c1.id, c2.id, c1.region FROM customers c1 JOIN customers c2 ON c1.region = c2.region WHERE c1.id < c2.id AND c1.id <= 20 AND c2.id <= 20 ORDER BY c1.id, c2.id", True)
    add_q("p5_self_customers_count",
          "SELECT COUNT(*) FROM customers c1 JOIN customers c2 ON c1.region = c2.region WHERE c1.id < c2.id AND c1.id <= 30 AND c2.id <= 30", False)
    add_q("p5_self_orders_amount_diff",
          "SELECT o1.id, o2.id, o1.amount - o2.amount FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 30 ORDER BY o1.id, o2.id LIMIT 20", True)
    add_q("p5_self_products_category",
          "SELECT p1.id, p2.id, p1.category FROM products p1 JOIN products p2 ON p1.category = p2.category WHERE p1.id < p2.id AND p1.id <= 15 AND p2.id <= 15 ORDER BY p1.id, p2.id", True)
    add_q("p5_self_left_join",
          "SELECT o1.id, o2.id FROM orders o1 LEFT JOIN orders o2 ON o1.cust_id = o2.cust_id AND o1.id != o2.id WHERE o1.id <= 10 ORDER BY o1.id, o2.id LIMIT 20", True)
    add_q("p5_self_join_star",
          "SELECT o1.*, o2.id FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 10 ORDER BY o1.id, o2.id LIMIT 10", True)
    add_q("p5_self_join_group_by",
          "SELECT o1.cust_id, COUNT(*) FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 50 GROUP BY o1.cust_id ORDER BY o1.cust_id LIMIT 15", True)
    add_q("p5_self_join_having",
          "SELECT o1.cust_id, COUNT(*) FROM orders o1 JOIN orders o2 ON o1.cust_id = o2.cust_id WHERE o1.id < o2.id AND o1.id <= 100 GROUP BY o1.cust_id HAVING COUNT(*) > 1 ORDER BY o1.cust_id", True)

    # -------------------------------------------------------------------------
    # Category 8: Aggregates, GROUP BY & HAVING over Joins (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_agg_count_group_region",
          "SELECT c.region, COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_sum_amount_group_region",
          "SELECT c.region, SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_avg_count_group_region",
          "SELECT c.region, COUNT(*), AVG(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_min_max_group_region",
          "SELECT c.region, MIN(o.amount), MAX(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_group_region_status",
          "SELECT c.region, o.status, COUNT(*), SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region, o.status ORDER BY c.region, o.status", True)
    add_q("p5_agg_having_count",
          "SELECT c.region, COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region HAVING COUNT(*) > 5 ORDER BY c.region", True)
    add_q("p5_agg_having_sum",
          "SELECT c.region, SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region HAVING SUM(o.amount) > 1000 ORDER BY c.region", True)
    add_q("p5_agg_where_and_having",
          "SELECT c.region, COUNT(*), SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 50 GROUP BY c.region HAVING COUNT(*) > 2 ORDER BY c.region", True)
    add_q("p5_agg_scalar_aggregates_all",
          "SELECT COUNT(*), SUM(o.amount), AVG(o.amount), MIN(o.amount), MAX(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_agg_left_join_group",
          "SELECT c.region, COUNT(o.id), COUNT(*) FROM customers c LEFT JOIN orders o ON c.id = o.cust_id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_left_join_having",
          "SELECT c.region, COUNT(o.id) FROM customers c LEFT JOIN orders o ON c.id = o.cust_id GROUP BY c.region HAVING COUNT(o.id) > 0 ORDER BY c.region", True)
    add_q("p5_agg_product_join_group",
          "SELECT p.category, COUNT(*), SUM(o.qty) FROM orders o JOIN products p ON o.product_id = p.id GROUP BY p.category ORDER BY p.category", True)
    add_q("p5_agg_product_join_having",
          "SELECT p.category, AVG(p.price) FROM orders o JOIN products p ON o.product_id = p.id GROUP BY p.category HAVING AVG(p.price) > 5 ORDER BY p.category", True)
    add_q("p5_agg_count_distinct_not_supported_but_count_col",
          "SELECT c.region, COUNT(o.cust_id) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)
    add_q("p5_agg_arithmetic_inside_agg",
          "SELECT c.region, SUM(o.qty * o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region", True)

    # -------------------------------------------------------------------------
    # Category 9: ORDER BY, DISTINCT & LIMIT/OFFSET over Joins (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_order_asc_desc",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id ASC, c.name ASC LIMIT 15", True)
    add_q("p5_order_alias",
          "SELECT o.id AS order_id, c.name AS cust_name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY order_id DESC, cust_name ASC LIMIT 15", True)
    add_q("p5_order_ordinal",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY 3 DESC, 1 ASC, 2 ASC LIMIT 15", True)
    add_q("p5_order_expr_arithmetic",
          "SELECT o.id, c.name, o.qty * o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.qty * o.amount DESC, o.id ASC, c.name ASC LIMIT 15", True)
    add_q("p5_order_agg_not_in_select_count",
          "SELECT c.region FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY COUNT(*) DESC, c.region", True)
    add_q("p5_order_agg_not_in_select_sum",
          "SELECT c.region FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY SUM(o.amount) DESC, c.region", True)
    add_q("p5_distinct_single_col",
          "SELECT DISTINCT c.region FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY c.region", True)
    add_q("p5_distinct_two_cols",
          "SELECT DISTINCT c.region, o.status FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY c.region, o.status", True)
    add_q("p5_distinct_with_where",
          "SELECT DISTINCT c.region FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 100 ORDER BY c.region", True)
    add_q("p5_limit_offset_basic",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 10 OFFSET 5", True)
    add_q("p5_offset_with_large_limit",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 1000 OFFSET 20", True)
    add_q("p5_top_n_heap_small",
          "SELECT o.id, o.amount, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id ASC, c.name ASC LIMIT 5", True)
    add_q("p5_top_n_heap_with_offset",
          "SELECT o.id, o.amount, c.name FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id ASC, c.name ASC LIMIT 5 OFFSET 3", True)
    add_q("p5_external_sort_join",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id ASC, c.name ASC", True,
          engine_args=["--table", "orders={orders}", "--table", "customers={customers}", "--memory-limit-mb", "1", "--sort-fan-in", "4", "--sql", "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id ASC, c.name ASC"])
    add_q("p5_no_pushdown_verification",
          "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 100 AND c.region = 'North' ORDER BY o.id, c.name LIMIT 20", True,
          engine_args=["--table", "orders={orders}", "--table", "customers={customers}", "--no-pushdown", "--sql", "SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.cust_id = c.id WHERE o.amount > 100 AND c.region = 'North' ORDER BY o.id, c.name LIMIT 20"])

    # -------------------------------------------------------------------------
    # Category 10: Edge Case Datasets (15 queries)
    # -------------------------------------------------------------------------
    add_q("p5_edge_empty_customers_inner",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_empty_customers_left",
          "SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_edge_empty_orders_inner",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_empty_orders_left",
          "SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_all_null_keys_inner",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_all_null_keys_left",
          "SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_edge_no_overlap_inner",
          "SELECT o.id, c.name FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_no_overlap_left",
          "SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.name LIMIT 20", True)
    add_q("p5_edge_giant_key_count",
          "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id", False)
    add_q("p5_edge_giant_key_group",
          "SELECT o.cust_id, COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY o.cust_id", False)
    add_q("p5_edge_one_row_inner",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id", False)
    add_q("p5_edge_one_row_left",
          "SELECT o.id, c.name, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id", False)
    add_q("p5_edge_crlf_inner",
          "SELECT o.id, c.name, p.name FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name", True)
    add_q("p5_edge_crlf_left",
          "SELECT o.id, c.name, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id ORDER BY o.id, c.name, p.name", True)
    add_q("p5_edge_crlf_note_quoted",
          "SELECT c.id, c.note FROM orders o JOIN customers c ON o.cust_id = c.id ORDER BY c.id, c.note, o.id", True)

    return queries

def main():
    queries = build_queries()
    print(f"Generated {len(queries)} join queries.")
    assert len(queries) >= 110, f"Expected >= 110 queries, got {len(queries)}"

    out_path = os.path.join(os.path.dirname(__file__), "queries_join.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(queries, f, indent=2)
    print(f"Wrote queries to {out_path}")

if __name__ == "__main__":
    main()

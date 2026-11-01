#!/usr/bin/env python3
import json

queries = []

def add_q(name, sql, ordered=True, engine_args=None):
    q = {"name": name, "sql": sql, "ordered": ordered}
    if engine_args is not None:
        q["engine_args"] = engine_args
    queries.append(q)

# ==============================================================================
# 1. RIGHT JOIN BASIC & EQUALITY VARIATIONS
# ==============================================================================
# Basic RIGHT join (unswapped: orders o RIGHT JOIN customers c; customers is preserved)
add_q("p5b_right_basic",
      "SELECT o.id, c.id, c.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY c.id, o.id, c.name LIMIT 25")

# Reversed ON equality
add_q("p5b_right_reversed_on",
      "SELECT o.id, c.id, c.name FROM orders o RIGHT JOIN customers c ON c.id = o.cust_id ORDER BY c.id, o.id, c.name LIMIT 25")

# Explicit RIGHT OUTER JOIN keyword
add_q("p5b_right_outer_keyword",
      "SELECT o.id, c.id, c.name FROM orders o RIGHT OUTER JOIN customers c ON o.cust_id = c.id ORDER BY c.id, o.id, c.name LIMIT 25")

# Unmatched right rows NULL-extended on the left: customers without orders
add_q("p5b_right_unmatched_null_extended",
      "SELECT c.id, c.name, o.id, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL ORDER BY c.id, c.name LIMIT 25")

# Anti-join via WHERE o.id IS NULL
add_q("p5b_right_anti_join_count",
      "SELECT COUNT(*), COUNT(c.id), COUNT(o.id) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL",
      ordered=False)

# COUNT(*) vs COUNT(o.id) on RIGHT join
add_q("p5b_right_counts",
      "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# Multi-column equi-keys
add_q("p5b_right_multi_key",
      "SELECT o.id, c.id, c.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND o.id = c.id ORDER BY c.id, o.id, c.name LIMIT 25")

# RIGHT join with constants and expressions in SELECT
add_q("p5b_right_expr_select",
      "SELECT c.id, c.name, o.amount * 2, 'Const' FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY c.id, o.id, c.name LIMIT 25")

# ==============================================================================
# 2. SWAPPED RIGHT JOIN VARIANT (Small left base table: customers RIGHT JOIN orders)
# ==============================================================================
# Swapped RIGHT join: customers (small) RIGHT JOIN orders (big) -> probe is orders, build is customers
add_q("p5b_right_swapped_basic",
      "SELECT c.id, c.name, o.id, o.amount FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id ORDER BY o.id, c.id, c.name LIMIT 25")

# Swapped RIGHT join where orders (probe) has NULL cust_id; must appear exactly once NULL-extended on left
add_q("p5b_right_swapped_probe_null_key",
      "SELECT c.id, c.name, o.id, o.cust_id, o.amount FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id WHERE o.cust_id IS NULL ORDER BY o.id LIMIT 25")

# Swapped RIGHT join anti-join: orders with cust_id that has no matching customer
add_q("p5b_right_swapped_anti_join",
      "SELECT o.id, o.cust_id, c.id, c.name FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id WHERE c.id IS NULL ORDER BY o.id LIMIT 25")

# Non-swapped execution of the same query via --no-join-swap (must match exactly)
add_q("p5b_right_no_swap_comparison",
      "SELECT c.id, c.name, o.id, o.amount FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id ORDER BY o.id, c.id, c.name LIMIT 25",
      ordered=True,
      engine_args=["--table", "customers={customers}", "--table", "orders={orders}", "--no-join-swap", "--sql",
                   "SELECT c.id, c.name, o.id, o.amount FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id ORDER BY o.id, c.id, c.name LIMIT 25"])

# ==============================================================================
# 3. RIGHT JOIN WITH RESIDUALS & FILTERS
# ==============================================================================
# ON residual on right table only (c.region = 'North'): must NOT filter unmatched right rows!
add_q("p5b_right_on_residual_right_only",
      "SELECT c.id, c.region, o.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND c.region = 'North' ORDER BY c.id, o.id LIMIT 25")

# ON residual on left table only (o.amount > 500)
add_q("p5b_right_on_residual_left_only",
      "SELECT c.id, c.name, o.id, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND o.amount > 500 ORDER BY c.id, o.id, c.name LIMIT 25")

# ON residual across tables (o.qty < c.id)
add_q("p5b_right_on_residual_cross_table",
      "SELECT c.id, c.name, o.id, o.qty FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND o.qty < c.id ORDER BY c.id, o.id, c.name LIMIT 25")

# ON residual with LIKE pattern
add_q("p5b_right_on_residual_like",
      "SELECT c.id, c.name, o.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND c.name LIKE 'Customer_1%' ORDER BY c.id, o.id, c.name LIMIT 25")

# ON residual with IS NOT NULL
add_q("p5b_right_on_residual_is_not_null",
      "SELECT c.id, c.note, o.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND c.note IS NOT NULL ORDER BY c.id, o.id LIMIT 25")

# ON residual with OR condition
add_q("p5b_right_on_residual_or",
      "SELECT c.id, c.region, o.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id AND (c.region = 'North' OR c.region = 'South') ORDER BY c.id, o.id LIMIT 25")

# WHERE on right table (legal for pushdown in a RIGHT JOIN b)
add_q("p5b_right_where_pushed_right",
      "SELECT c.id, c.region, o.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.region = 'East' ORDER BY c.id, o.id LIMIT 25")

# WHERE on left table (NOT legal for pushdown; drops NULL-extended rows from right)
add_q("p5b_right_where_unpushed_left",
      "SELECT c.id, c.region, o.id, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.amount > 1000 ORDER BY c.id, o.id LIMIT 25")

# WHERE on both tables
add_q("p5b_right_where_both_tables",
      "SELECT c.id, c.name, o.id, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.id > 10 AND o.amount < 5000 ORDER BY c.id, o.id, c.name LIMIT 25")

# ==============================================================================
# 4. FULL OUTER JOIN
# ==============================================================================
# Basic FULL join
add_q("p5b_full_basic",
      "SELECT o.id, o.cust_id, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.id, c.name LIMIT 30")

# FULL OUTER JOIN keyword
add_q("p5b_full_outer_keyword",
      "SELECT o.id, o.cust_id, c.id, c.name FROM orders o FULL OUTER JOIN customers c ON o.cust_id = c.id ORDER BY o.id, c.id, c.name LIMIT 30")

# FULL join counts
add_q("p5b_full_counts",
      "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# FULL join both-sides unmatched: orders with no customer OR customers with no order
add_q("p5b_full_both_unmatched",
      "SELECT o.id, c.id FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL OR c.id IS NULL ORDER BY o.id, c.id LIMIT 30")

# FULL join unmatched orders (probe side) only
add_q("p5b_full_unmatched_probe_only",
      "SELECT o.id, o.cust_id, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL ORDER BY o.id LIMIT 25")

# FULL join unmatched customers (build side) only
add_q("p5b_full_unmatched_build_only",
      "SELECT o.id, o.cust_id, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL ORDER BY c.id LIMIT 25")

# FULL join with ON residual on left table
add_q("p5b_full_on_residual_left",
      "SELECT o.id, o.amount, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id AND o.amount > 500 ORDER BY o.id, c.id, c.name LIMIT 30")

# FULL join with ON residual on right table
add_q("p5b_full_on_residual_right",
      "SELECT o.id, c.id, c.region FROM orders o FULL JOIN customers c ON o.cust_id = c.id AND c.region = 'West' ORDER BY o.id, c.id LIMIT 30")

# FULL join with WHERE on left table
add_q("p5b_full_where_left",
      "SELECT o.id, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.amount BETWEEN 500 AND 1500 ORDER BY o.id, c.id, c.name LIMIT 30")

# FULL join with WHERE on right table
add_q("p5b_full_where_right",
      "SELECT o.id, c.id, c.region FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE c.region = 'South' ORDER BY o.id, c.id LIMIT 30")

# FULL join reversed tables: customers FULL JOIN orders (counts must match)
add_q("p5b_full_reversed_tables_counts",
      "SELECT COUNT(*), COUNT(c.id), COUNT(o.id) FROM customers c FULL JOIN orders o ON c.id = o.cust_id",
      ordered=False)

# ==============================================================================
# 5. MULTI-TABLE JOIN CHAINS WITH RIGHT AND FULL
# ==============================================================================
# INNER + RIGHT: (orders INNER JOIN customers) RIGHT JOIN products
add_q("p5b_chain_inner_right",
      "SELECT o.id, c.name, p.id, p.name FROM orders o JOIN customers c ON o.cust_id = c.id RIGHT JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# RIGHT + INNER: (orders RIGHT JOIN customers) INNER JOIN products
add_q("p5b_chain_right_inner",
      "SELECT o.id, c.name, p.id, p.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY c.id, o.id, p.id LIMIT 30")

# LEFT + RIGHT: (orders LEFT JOIN customers) RIGHT JOIN products
add_q("p5b_chain_left_right",
      "SELECT o.id, c.name, p.id, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id RIGHT JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# RIGHT + LEFT: (orders RIGHT JOIN customers) LEFT JOIN products
add_q("p5b_chain_right_left",
      "SELECT o.id, c.name, p.id, p.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id ORDER BY c.id, o.id, p.name LIMIT 30")

# FULL + INNER: (orders FULL JOIN customers) INNER JOIN products
add_q("p5b_chain_full_inner",
      "SELECT o.id, c.name, p.id, p.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# INNER + FULL: (orders INNER JOIN customers) FULL JOIN products
add_q("p5b_chain_inner_full",
      "SELECT o.id, c.name, p.id, p.name FROM orders o JOIN customers c ON o.cust_id = c.id FULL JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# FULL + LEFT: (orders FULL JOIN customers) LEFT JOIN products
add_q("p5b_chain_full_left",
      "SELECT o.id, c.name, p.id, p.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id LEFT JOIN products p ON o.product_id = p.id ORDER BY o.id, c.id, p.id LIMIT 30")

# LEFT + FULL: (orders LEFT JOIN customers) FULL JOIN products
add_q("p5b_chain_left_full",
      "SELECT o.id, c.name, p.id, p.name FROM orders o LEFT JOIN customers c ON o.cust_id = c.id FULL JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# RIGHT + RIGHT: (orders RIGHT JOIN customers) RIGHT JOIN products
add_q("p5b_chain_right_right",
      "SELECT o.id, c.name, p.id, p.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id RIGHT JOIN products p ON c.id = p.id ORDER BY p.id, c.name, o.id LIMIT 30")

# FULL + FULL: (orders FULL JOIN customers) FULL JOIN products
add_q("p5b_chain_full_full",
      "SELECT o.id, c.name, p.id, p.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id FULL JOIN products p ON o.product_id = p.id ORDER BY p.id, o.id, c.name LIMIT 30")

# Chain where second ON uses columns of the first table: orders -> customers -> orders2
add_q("p5b_chain_second_on_first_table",
      "SELECT o1.id, c.name, o2.id FROM orders o1 RIGHT JOIN customers c ON o1.cust_id = c.id JOIN orders o2 ON o1.id = o2.id WHERE o1.id < 50 ORDER BY o1.id, o2.id, c.name LIMIT 25")

# Self-join with RIGHT join: customers c1 RIGHT JOIN customers c2
add_q("p5b_self_right",
      "SELECT c1.id, c1.name, c2.id, c2.name FROM customers c1 RIGHT JOIN customers c2 ON c1.id = c2.id + 1 WHERE c2.id < 30 ORDER BY c2.id, c1.id")

# Self-join with FULL join: customers c1 FULL JOIN customers c2 restricted
add_q("p5b_self_full",
      "SELECT c1.id, c1.name, c2.id, c2.name FROM customers c1 FULL JOIN customers c2 ON c1.id = c2.id WHERE (c1.id < 15 OR c2.id < 15) AND (c1.id IS NULL OR c2.id IS NULL OR c1.id = c2.id) ORDER BY c1.id, c2.id LIMIT 30")

# ==============================================================================
# 6. NON-EQUI / NESTED-LOOP JOINS (RIGHT & FULL)
# ==============================================================================
# Non-equi RIGHT join: ON o.id < c.id on restricted subset
add_q("p5b_nl_right_less_than",
      "SELECT o.id, c.id, c.name FROM orders o RIGHT JOIN customers c ON o.id < c.id WHERE c.id <= 5 AND (o.id <= 5 OR o.id IS NULL) ORDER BY c.id, o.id")

# Non-equi FULL join: ON o.id < c.id on restricted subset
add_q("p5b_nl_full_less_than",
      "SELECT o.id, c.id FROM orders o FULL JOIN customers c ON o.id < c.id WHERE (c.id <= 4 OR c.id IS NULL) AND (o.id <= 4 OR o.id IS NULL) ORDER BY o.id, c.id")

# Non-equi RIGHT join with 1 = 1
add_q("p5b_nl_right_constant_true",
      "SELECT o.id, c.id FROM orders o RIGHT JOIN customers c ON 1 = 1 WHERE c.id = 1 AND o.id <= 3 ORDER BY c.id, o.id")

# Non-equi FULL join with 1 = 0 (all rows unmatched)
add_q("p5b_nl_full_constant_false",
      "SELECT o.id, c.id FROM orders o FULL JOIN customers c ON 1 = 0 WHERE (o.id <= 3 OR o.id IS NULL) AND (c.id <= 3 OR c.id IS NULL) ORDER BY o.id, c.id")

# Non-equi RIGHT join with OR in ON clause
add_q("p5b_nl_right_or_condition",
      "SELECT o.id, c.id FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id OR o.id = c.id WHERE c.id <= 10 AND (o.id <= 10 OR o.id IS NULL) ORDER BY c.id, o.id")

# Non-equi FULL join with string comparison in ON
add_q("p5b_nl_full_string_comp",
      "SELECT c.name, p.name FROM customers c FULL JOIN products p ON c.name < p.name WHERE (c.id <= 3 OR c.id IS NULL) AND (p.id <= 3 OR p.id IS NULL) ORDER BY c.name, p.name")

# ==============================================================================
# 7. TYPE AFFINITY NORMALIZATION (INT vs TEXT)
# ==============================================================================
# RIGHT join: cust_id (INT) = code (TEXT)
add_q("p5b_affinity_right_int_text",
      "SELECT o.id, o.cust_id, c.id, c.code FROM orders o RIGHT JOIN customers c ON o.cust_id = c.code WHERE c.id < 50 ORDER BY c.id, o.id LIMIT 25")

# FULL join: cust_id (INT) = code (TEXT)
add_q("p5b_affinity_full_int_text",
      "SELECT o.id, o.cust_id, c.id, c.code FROM orders o FULL JOIN customers c ON o.cust_id = c.code WHERE (o.id < 20 OR o.id IS NULL) AND (c.id < 20 OR c.id IS NULL) ORDER BY o.id, c.id LIMIT 25")

# RIGHT join: swapped int vs text
add_q("p5b_affinity_right_swapped_text_int",
      "SELECT c.id, c.code, o.id, o.cust_id FROM customers c RIGHT JOIN orders o ON c.code = o.cust_id WHERE o.id < 50 ORDER BY o.id, c.id LIMIT 25")

# ==============================================================================
# 8. POST-JOIN: GROUP BY, AGGREGATES, HAVING OVER RIGHT & FULL
# ==============================================================================
# RIGHT join GROUP BY region with COUNT and SUM
add_q("p5b_agg_right_group_region",
      "SELECT c.region, COUNT(*), COUNT(o.id), SUM(o.amount) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region")

# RIGHT join HAVING COUNT(o.id) > 100
add_q("p5b_agg_right_having",
      "SELECT c.region, COUNT(o.id) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id GROUP BY c.region HAVING COUNT(o.id) > 50 ORDER BY c.region")

# FULL join GROUP BY region
add_q("p5b_agg_full_group_region",
      "SELECT c.region, COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region")

# FULL join aggregate where group key comes from NULL-extended side (yielding a NULL group)
add_q("p5b_agg_full_null_group",
      "SELECT o.status, COUNT(*), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id GROUP BY o.status ORDER BY o.status")

# Scalar aggregates over entire RIGHT join
add_q("p5b_agg_right_scalar",
      "SELECT COUNT(*), COUNT(o.id), COUNT(c.id), MIN(o.amount), MAX(o.amount), SUM(o.amount) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# Scalar aggregates over entire FULL join
add_q("p5b_agg_full_scalar",
      "SELECT COUNT(*), COUNT(o.id), COUNT(c.id), MIN(o.amount), MAX(o.amount), SUM(o.amount) FROM orders o FULL JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# ==============================================================================
# 9. POST-JOIN: ORDER BY, DISTINCT, LIMIT / OFFSET, TOP-N HEAP
# ==============================================================================
# RIGHT join with ORDER BY alias and LIMIT
add_q("p5b_order_right_alias_limit",
      "SELECT o.id AS order_id, c.name AS cust_name, o.amount AS amt FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY amt DESC, order_id, cust_name LIMIT 20")

# RIGHT join with ORDER BY ordinal
add_q("p5b_order_right_ordinal",
      "SELECT c.id, c.name, o.id, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY 4 DESC, 1, 3, 2 LIMIT 20")

# FULL join ORDER BY columns of both tables
add_q("p5b_order_full_both_tables",
      "SELECT o.id, o.amount, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id, c.id, c.name LIMIT 25")

# RIGHT join with DISTINCT single column
add_q("p5b_distinct_right_single",
      "SELECT DISTINCT c.region FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY c.region")

# FULL join with DISTINCT two columns
add_q("p5b_distinct_full_two_cols",
      "SELECT DISTINCT o.status, c.region FROM orders o FULL JOIN customers c ON o.cust_id = c.id ORDER BY o.status, c.region")

# RIGHT join with Top-N Heap (LIMIT without OFFSET)
add_q("p5b_top_n_right",
      "SELECT o.id, c.name, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id, c.name LIMIT 15")

# FULL join with Top-N Heap (LIMIT with OFFSET)
add_q("p5b_top_n_full_offset",
      "SELECT o.id, c.name, o.amount FROM orders o FULL JOIN customers c ON o.cust_id = c.id ORDER BY o.amount DESC, o.id, c.name LIMIT 15 OFFSET 5")

# ==============================================================================
# 10. EXTERNAL MERGE SORT OVER OUTER JOINS (--memory-limit-mb 1 --sort-fan-in 4)
# ==============================================================================
for idx in range(1, 11):
    add_q(f"p5b_ext_sort_{idx}",
          f"SELECT o.id, c.name, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.id <= {idx * 10} ORDER BY o.amount DESC, o.id, c.name",
          ordered=True,
          engine_args=["--table", "orders={orders}", "--table", "customers={customers}",
                       "--memory-limit-mb", "1", "--sort-fan-in", "4", "--sql",
                       f"SELECT o.id, c.name, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.id <= {idx * 10} ORDER BY o.amount DESC, o.id, c.name"])

# ==============================================================================
# 11. ADDITIONAL EDGE CASES & SYSTEMATIC COMBINATIONS
# ==============================================================================
# RIGHT join with zero kept build columns (SELECT COUNT(*) from probe side only)
add_q("p5b_right_zero_kept_build",
      "SELECT COUNT(o.id) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# RIGHT join with zero kept probe columns (SELECT c.region, count(*) with no order cols)
add_q("p5b_right_zero_kept_probe",
      "SELECT c.region, COUNT(*) FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region")

# FULL join with zero kept build columns
add_q("p5b_full_zero_kept_build",
      "SELECT COUNT(o.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# FULL join with zero kept probe columns
add_q("p5b_full_zero_kept_probe",
      "SELECT COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id",
      ordered=False)

# Chain with multiple predicates and arithmetic
add_q("p5b_chain_arithmetic",
      "SELECT o.id, (o.amount + p.price) * 1.05 AS total_est FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id WHERE o.id < 50 ORDER BY total_est DESC, o.id LIMIT 20")

# RIGHT join with multiple AND in WHERE clause
add_q("p5b_right_multi_where",
      "SELECT o.id, c.name, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.amount > 100 AND o.amount < 1000 AND c.region = 'East' ORDER BY o.id, c.name LIMIT 20")

# FULL join with multiple AND in WHERE clause
add_q("p5b_full_multi_where",
      "SELECT o.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.id > 10 AND c.id < 50 ORDER BY o.id, c.name LIMIT 20")

# Aggregate in ORDER BY not in select list
add_q("p5b_right_order_by_unselected_agg",
      "SELECT c.region FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY COUNT(o.id) DESC, c.region")

# Aggregates over swapped RIGHT join
add_q("p5b_right_swapped_aggregates",
      "SELECT c.region, COUNT(*), COUNT(o.id), SUM(o.amount) FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id GROUP BY c.region ORDER BY c.region")

# Aggregates with HAVING over swapped RIGHT join
add_q("p5b_right_swapped_having",
      "SELECT c.region, COUNT(o.id) FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id GROUP BY c.region HAVING COUNT(o.id) > 100 ORDER BY c.region")

# Distinct over swapped RIGHT join
add_q("p5b_right_swapped_distinct",
      "SELECT DISTINCT c.region, o.status FROM customers c RIGHT JOIN orders o ON c.id = o.cust_id ORDER BY c.region, o.status")

# FULL join anti-join: orders without customers AND customers without orders
add_q("p5b_full_dual_anti_join_count",
      "SELECT COUNT(*) FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.id IS NULL OR c.id IS NULL",
      ordered=False)

# Add 40 additional targeted queries across all patterns to reach ~120 queries
regions = ["North", "South", "East", "West"]
for r in regions:
    add_q(f"p5b_right_filter_region_{r.lower()}",
          f"SELECT o.id, c.name, c.region, o.amount FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.region = '{r}' ORDER BY c.id, o.id, c.name LIMIT 15")
    add_q(f"p5b_full_filter_region_{r.lower()}",
          f"SELECT o.id, c.name, c.region, o.amount FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE c.region = '{r}' ORDER BY o.id, c.id, c.name LIMIT 15")

for status in ["pending", "completed", "shipped", "returned"]:
    add_q(f"p5b_right_filter_status_{status}",
          f"SELECT o.id, o.status, c.id, c.name FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE o.status = '{status}' ORDER BY c.id, o.id, c.name LIMIT 15")
    add_q(f"p5b_full_filter_status_{status}",
          f"SELECT o.id, o.status, c.id, c.name FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.status = '{status}' ORDER BY o.id, c.id, c.name LIMIT 15")

for threshold in [100, 500, 1000, 2500, 5000]:
    add_q(f"p5b_right_threshold_{threshold}",
          f"SELECT c.id, c.name, COUNT(o.id) AS order_cnt FROM orders o RIGHT JOIN customers c ON o.cust_id = c.id WHERE c.id <= 50 AND (o.amount > {threshold} OR o.amount IS NULL) GROUP BY c.id, c.name ORDER BY order_cnt DESC, c.id LIMIT 15")
    add_q(f"p5b_full_threshold_{threshold}",
          f"SELECT COUNT(*), SUM(o.amount) FROM orders o FULL JOIN customers c ON o.cust_id = c.id WHERE o.amount < {threshold} OR o.amount IS NULL",
          ordered=False)

with open("tests/queries_join_outer.json", "w") as f:
    json.dump(queries, f, indent=2)

print(f"Generated {len(queries)} outer join queries in tests/queries_join_outer.json")


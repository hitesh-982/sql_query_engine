#!/usr/bin/env python3
"""
gen_data.py - Data generator for SQL query engine testing.
Streams rows to disk in chunks to handle multi-GB files without high memory usage.
"""

import argparse
import csv
import os
import random
import sys


def get_zipf_weights(n, s=1.0):
    # Zipf weights: w_i = 1 / (i^s)
    return [1.0 / (i ** s) for i in range(1, n + 1)]


def generate_default_table(out_path, rows, seed):
    random.seed(seed)
    
    # ~1000 distinct cities with Zipf skew
    num_cities = 1000
    cities = [f"City_{i}" for i in range(1, num_cities + 1)]
    city_weights = get_zipf_weights(num_cities, s=1.0)
    
    # ~10 distinct regions
    regions = [
        "North", "South", "East", "West", "Central",
        "Northeast", "Northwest", "Southeast", "Southwest", "Midwest"
    ]
    
    note_templates = [
        "standard note",
        "note with, comma included",
        'note with "quotes" inside',
        'note, with both, "quotes" and, commas',
        'He said, "urgent order, please expedite!"',
        "plain description",
        "",
        'double ""quotes"" test',
        "simple transaction note",
        "verified, OK"
    ]

    header = ["city", "region", "id", "qty", "amount", "note"]
    
    batch_size = 10000
    rows_generated = 0
    
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        
        while rows_generated < rows:
            current_batch = min(batch_size, rows - rows_generated)
            # Batch generate skewed cities
            batch_cities = random.choices(cities, weights=city_weights, k=current_batch)
            batch_regions = random.choices(regions, k=current_batch)
            
            for b_idx in range(current_batch):
                row_id = rows_generated + b_idx + 1
                
                # ~1% city empty (NULL)
                city = "" if random.random() < 0.01 else batch_cities[b_idx]
                region = batch_regions[b_idx]
                
                # ~5% qty empty (NULL)
                qty = "" if random.random() < 0.05 else str(random.randint(1, 1000))
                
                # ~5% amount empty (NULL)
                amount = "" if random.random() < 0.05 else f"{random.uniform(0.50, 9999.50):.2f}"
                
                note = random.choice(note_templates)
                
                writer.writerow([city, region, row_id, qty, amount, note])
                
            rows_generated += current_batch


def generate_customers_table(out_path, rows, seed):
    random.seed(seed)
    
    regions = [
        "North", "South", "East", "West", "Central",
        "Northeast", "Northwest", "Southeast", "Southwest", "Midwest"
    ]
    
    header = ["id", "name", "region"]
    batch_size = 10000
    rows_generated = 0
    
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        
        while rows_generated < rows:
            current_batch = min(batch_size, rows - rows_generated)
            for b_idx in range(current_batch):
                curr_idx = rows_generated + b_idx + 1
                
                # ~5% NULL id, ~10% duplicate id (reuse a recent id)
                rand_val = random.random()
                if rand_val < 0.05:
                    cust_id = ""
                elif rand_val < 0.15 and curr_idx > 10:
                    cust_id = str(random.randint(max(1, curr_idx - 50), curr_idx - 1))
                else:
                    cust_id = str(curr_idx)
                    
                name = f"Customer_{curr_idx}"
                region = random.choice(regions)
                
                writer.writerow([cust_id, name, region])
                
            rows_generated += current_batch


def generate_edge_files(edge_dir, seed):
    os.makedirs(edge_dir, exist_ok=True)
    random.seed(seed)
    
    header = ["city", "region", "id", "qty", "amount", "note"]
    
    # 1. empty_header_only.csv
    with open(os.path.join(edge_dir, "empty_header_only.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        
    # 2. one_row.csv
    with open(os.path.join(edge_dir, "one_row.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        writer.writerow(["City_1", "East", 1, 10, "99.95", 'single, "quoted" note'])
        
    # 3. all_null_column.csv (qty is always empty/NULL)
    with open(os.path.join(edge_dir, "all_null_column.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        for i in range(1, 51):
            writer.writerow([f"City_{i}", "West", i, "", f"{i * 1.50:.2f}", "all null qty"])
            
    # 4. one_giant_group.csv (city & region identical for all rows)
    with open(os.path.join(edge_dir, "one_giant_group.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(header)
        for i in range(1, 101):
            writer.writerow(["Metropolis", "Central", i, 5, "10.00", "group note"])
            
    # 5. crlf.csv (\r\n line endings)
    with open(os.path.join(edge_dir, "crlf.csv"), "wb") as f:
        lines = [
            "city,region,id,qty,amount,note\r\n",
            "City_1,East,1,10,12.50,crlf note 1\r\n",
            "City_2,West,2,,25.00,\"crlf, note 2\"\r\n",
            "City_3,North,3,30,,crlf note 3\r\n"
        ]
        for line in lines:
            f.write(line.encode("utf-8"))
            
    # 6. no_trailing_newline.csv (no newline at EOF)
    with open(os.path.join(edge_dir, "no_trailing_newline.csv"), "wb") as f:
        content = (
            "city,region,id,qty,amount,note\n"
            "City_1,East,1,10,12.50,first\n"
            "City_2,West,2,20,25.00,second_no_newline"
        )
        f.write(content.encode("utf-8"))


def generate_join_tables(out_dir, num_orders, num_customers, num_products, seed, generate_edges=True):
    os.makedirs(out_dir, exist_ok=True)
    random.seed(seed)
    
    regions = [
        "North", "South", "East", "West", "Central",
        "Northeast", "Northwest", "Southeast", "Southwest", "Midwest"
    ]
    
    codes_numeric = ["7", "007", " 12 ", "7.0", " 7 ", "100", "0", "42.0", "42"]
    codes_non_numeric = ["abc", "x7", "xyz", "7a", "N/A"]
    
    note_templates = [
        "standard note",
        "note with, comma included",
        'note with "quotes" inside',
        'note, with both, "quotes" and, commas',
        'He said, "urgent order, please expedite!"',
        "plain description",
        "",
        'double ""quotes"" test',
        "simple transaction note",
        "verified, OK"
    ]
    
    statuses = ["pending", "shipped", "delivered", "cancelled", "returned"]
    categories = ["Electronics", "Clothing", "Home", "Books", "Sports", "Toys", "Automotive", "Garden"]
    
    # 1. Generate products.csv
    products_path = os.path.join(out_dir, "products.csv")
    with open(products_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["id", "name", "price", "category"])
        batch_size = 10000
        generated = 0
        while generated < num_products:
            batch = min(batch_size, num_products - generated)
            for b in range(batch):
                pid = generated + b + 1
                name = f"Product_{pid}"
                price = "" if random.random() < 0.03 else f"{random.uniform(1.0, 500.0):.2f}"
                cat = random.choice(categories)
                writer.writerow([pid, name, price, cat])
            generated += batch

    # 2. Generate customers.csv
    # id with ~2% duplicates, ~1% empty
    customers_path = os.path.join(out_dir, "customers.csv")
    with open(customers_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["id", "name", "region", "code", "note"])
        batch_size = 10000
        generated = 0
        while generated < num_customers:
            batch = min(batch_size, num_customers - generated)
            for b in range(batch):
                curr_idx = generated + b + 1
                r = random.random()
                if r < 0.01:
                    cid = ""
                elif r < 0.03 and curr_idx > 5:
                    cid = str(random.randint(max(1, curr_idx - 20), curr_idx - 1))
                else:
                    cid = str(curr_idx)
                
                name = f"Customer_{curr_idx}"
                reg = "" if random.random() < 0.01 else random.choice(regions)
                
                # code for affinity tests: numeric-looking strings and non-numeric
                if random.random() < 0.7:
                    code_val = random.choice(codes_numeric)
                else:
                    code_val = random.choice(codes_non_numeric)
                    
                note_val = random.choice(note_templates)
                writer.writerow([cid, name, reg, code_val, note_val])
            generated += batch

    # 3. Generate orders.csv
    # cust_id ~4% empty, ~3% dangling, skewed so some customers have many orders
    orders_path = os.path.join(out_dir, "orders.csv")
    cust_weights = get_zipf_weights(min(num_customers, 5000), s=1.1)
    with open(orders_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["id", "cust_id", "product_id", "qty", "amount", "status"])
        batch_size = 10000
        generated = 0
        while generated < num_orders:
            batch = min(batch_size, num_orders - generated)
            skewed_custs = random.choices(range(1, len(cust_weights) + 1), weights=cust_weights, k=batch)
            for b in range(batch):
                oid = generated + b + 1
                r = random.random()
                if r < 0.04:
                    cust_id = ""
                elif r < 0.07:
                    cust_id = str(num_customers + random.randint(1000, 999999)) # dangling id
                else:
                    cust_id = str(skewed_custs[b])
                    
                prod_id = "" if random.random() < 0.02 else str(random.randint(1, max(1, num_products)))
                qty = "" if random.random() < 0.05 else str(random.randint(1, 100))
                amount = "" if random.random() < 0.05 else f"{random.uniform(0.50, 9999.50):.2f}"
                stat = "" if random.random() < 0.02 else random.choice(statuses)
                writer.writerow([oid, cust_id, prod_id, qty, amount, stat])
            generated += batch

    if generate_edges:
        edge_dir = os.path.join(out_dir, "edge")
        generate_join_edge_files(edge_dir, seed)


def generate_join_edge_files(edge_dir, seed):
    os.makedirs(edge_dir, exist_ok=True)
    random.seed(seed)
    
    # Common products content for edges
    prod_header = ["id", "name", "price", "category"]
    cust_header = ["id", "name", "region", "code", "note"]
    ord_header = ["id", "cust_id", "product_id", "qty", "amount", "status"]

    def write_standard_products(d):
        with open(os.path.join(d, "products.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(prod_header)
            w.writerow([1, "Widget", "19.99", "Hardware"])
            w.writerow([2, "Gadget", "29.99", "Hardware"])

    def write_standard_customers(d):
        with open(os.path.join(d, "customers.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(cust_header)
            w.writerow([1, "Alice", "North", "7", "Alice note"])
            w.writerow([2, "Bob", "South", "007", "Bob note"])
            w.writerow([3, "Charlie", "East", "abc", "Charlie note"])

    def write_standard_orders(d):
        with open(os.path.join(d, "orders.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(ord_header)
            w.writerow([1, 1, 1, 5, "99.95", "shipped"])
            w.writerow([2, 2, 2, 10, "299.90", "pending"])
            w.writerow([3, 1, 2, 1, "29.99", "shipped"])

    # 1. empty_customers
    d1 = os.path.join(edge_dir, "empty_customers")
    os.makedirs(d1, exist_ok=True)
    with open(os.path.join(d1, "customers.csv"), "w", newline="", encoding="utf-8") as f:
        csv.writer(f, lineterminator="\n").writerow(cust_header)
    write_standard_orders(d1)
    write_standard_products(d1)

    # 2. empty_orders
    d2 = os.path.join(edge_dir, "empty_orders")
    os.makedirs(d2, exist_ok=True)
    with open(os.path.join(d2, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        csv.writer(f, lineterminator="\n").writerow(ord_header)
    write_standard_customers(d2)
    write_standard_products(d2)

    # 3. all_empty_orders_cust_id
    d3 = os.path.join(edge_dir, "all_empty_orders_cust_id")
    os.makedirs(d3, exist_ok=True)
    with open(os.path.join(d3, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(ord_header)
        for i in range(1, 21):
            w.writerow([i, "", 1, 2, "39.98", "pending"])
    write_standard_customers(d3)
    write_standard_products(d3)

    # 4. no_overlapping_keys
    d4 = os.path.join(edge_dir, "no_overlapping_keys")
    os.makedirs(d4, exist_ok=True)
    with open(os.path.join(d4, "customers.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cust_header)
        for i in range(1, 11):
            w.writerow([i, f"Cust_{i}", "North", "7", "note"])
    with open(os.path.join(d4, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(ord_header)
        for i in range(1, 11):
            w.writerow([i, 9000 + i, 1, 5, "50.00", "delivered"])
    write_standard_products(d4)

    # 5. one_giant_key (200 orders with same cust_id, 50 customers with that id)
    d5 = os.path.join(edge_dir, "one_giant_key")
    os.makedirs(d5, exist_ok=True)
    with open(os.path.join(d5, "customers.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cust_header)
        for i in range(1, 51):
            w.writerow([42, f"DupCustomer_{i}", "Central", "42", f"giant note {i}"])
        w.writerow([99, "OtherCustomer", "West", "99", "other"])
    with open(os.path.join(d5, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(ord_header)
        for i in range(1, 201):
            w.writerow([i, 42, 1, 1, "10.00", "shipped"])
        w.writerow([201, 88, 1, 2, "20.00", "pending"])
    write_standard_products(d5)

    # 6. one_row_tables
    d6 = os.path.join(edge_dir, "one_row_tables")
    os.makedirs(d6, exist_ok=True)
    with open(os.path.join(d6, "customers.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cust_header)
        w.writerow([1, "SoloCust", "East", "7.0", 'one, "row" note'])
    with open(os.path.join(d6, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(ord_header)
        w.writerow([1, 1, 1, 10, "100.00", "completed"])
    with open(os.path.join(d6, "products.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(prod_header)
        w.writerow([1, "SoloProduct", "10.00", "Books"])

    # 7. crlf_tables
    d7 = os.path.join(edge_dir, "crlf_tables")
    os.makedirs(d7, exist_ok=True)
    with open(os.path.join(d7, "customers.csv"), "wb") as f:
        content = (
            "id,name,region,code,note\r\n"
            "1,Alice,North,7,note1\r\n"
            "2,Bob,South,007,\"crlf, note\"\r\n"
        )
        f.write(content.encode("utf-8"))
    with open(os.path.join(d7, "orders.csv"), "wb") as f:
        content = (
            "id,cust_id,product_id,qty,amount,status\r\n"
            "1,1,1,5,25.00,shipped\r\n"
            "2,2,2,10,50.00,pending\r\n"
        )
        f.write(content.encode("utf-8"))
    with open(os.path.join(d7, "products.csv"), "wb") as f:
        content = (
            "id,name,price,category\r\n"
            "1,Widget,5.00,Tools\r\n"
            "2,Gadget,5.00,Tools\r\n"
        )
        f.write(content.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic CSV datasets for testing.")
    parser.add_argument("--rows", type=int, default=1000, help="Number of data rows")
    parser.add_argument("--out", type=str, default=None, help="Output CSV file path")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--edge", type=str, default=None, help="Directory to write edge case files")
    parser.add_argument("--table", type=str, default="default", choices=["default", "customers"],
                        help="Table schema to generate (default or customers)")
    parser.add_argument("--join-tables", type=str, default=None, help="Directory to output join test tables (orders, customers, products)")
    parser.add_argument("--orders", type=int, default=10000, help="Number of orders to generate")
    parser.add_argument("--customers", type=int, default=1000, help="Number of customers to generate")
    parser.add_argument("--products", type=int, default=100, help="Number of products to generate")
    parser.add_argument("--no-edges", action="store_true", help="Do not generate edge sets")
    args = parser.parse_args()
    
    if args.join_tables:
        generate_join_tables(args.join_tables, args.orders, args.customers, args.products, args.seed, not args.no_edges)
        return

    if not args.out:
        parser.error("--out is required when --join-tables is not specified")

    # Ensure parent directory of output file exists
    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        
    if args.table == "customers":
        generate_customers_table(args.out, args.rows, args.seed)
    else:
        generate_default_table(args.out, args.rows, args.seed)
        
    if args.edge:
        generate_edge_files(args.edge, args.seed)


if __name__ == "__main__":
    main()

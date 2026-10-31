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


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic CSV datasets for testing.")
    parser.add_argument("--rows", type=int, default=1000, help="Number of data rows")
    parser.add_argument("--out", type=str, required=True, help="Output CSV file path")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--edge", type=str, default=None, help="Directory to write edge case files")
    parser.add_argument("--table", type=str, default="default", choices=["default", "customers"],
                        help="Table schema to generate (default or customers)")
    args = parser.parse_args()
    
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

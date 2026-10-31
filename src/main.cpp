#include "csv_scan.h"
#include "sql_parser.h"
#include "planner.h"
#include "value.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>

static void printHeaderField(std::string_view field) {
    Value v = makeString(field);
    formatValue(v, stdout);
}

int main(int argc, char* argv[]) {
    std::string file_path;
    std::string select_cols;
    std::string table_arg;
    std::string sql_query;
    bool mode_select = false;
    bool mode_schema = false;
    bool mode_count = false;
    bool mode_sql = false;
    bool opt_explain = false;
    bool opt_no_output = false;
    size_t opt_max_groups = 0;
    size_t opt_memory_limit_mb = 512;
    std::string opt_tmp_dir;
    const char* env_tmp = std::getenv("TMPDIR");
    if (env_tmp && env_tmp[0] != '\0') {
        opt_tmp_dir = env_tmp;
    } else {
        opt_tmp_dir = "/tmp";
    }
    bool opt_sort_stats = false;
    size_t opt_sort_fan_in = 64;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "--select" && i + 1 < argc) {
            mode_select = true;
            select_cols = argv[++i];
        } else if (arg == "--schema") {
            mode_schema = true;
        } else if (arg == "--count") {
            mode_count = true;
        } else if (arg == "--table" && i + 1 < argc) {
            table_arg = argv[++i];
        } else if (arg == "--sql" && i + 1 < argc) {
            mode_sql = true;
            sql_query = argv[++i];
        } else if (arg == "--explain") {
            opt_explain = true;
        } else if (arg == "--no-output") {
            opt_no_output = true;
        } else if (arg == "--max-groups" && i + 1 < argc) {
            opt_max_groups = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--memory-limit-mb" && i + 1 < argc) {
            std::string val_str = argv[++i];
            try {
                size_t pos = 0;
                long long val = std::stoll(val_str, &pos);
                if (pos != val_str.size() || val <= 0) {
                    std::cerr << "Error: --memory-limit-mb must be a positive integer (got '" << val_str << "')\n";
                    return 2;
                }
                opt_memory_limit_mb = static_cast<size_t>(val);
            } catch (...) {
                std::cerr << "Error: --memory-limit-mb must be a positive integer (got '" << val_str << "')\n";
                return 2;
            }
        } else if (arg == "--tmp-dir" && i + 1 < argc) {
            opt_tmp_dir = argv[++i];
        } else if (arg == "--sort-stats") {
            opt_sort_stats = true;
        } else if (arg == "--sort-fan-in" && i + 1 < argc) {
            std::string val_str = argv[++i];
            try {
                size_t pos = 0;
                long long val = std::stoll(val_str, &pos);
                if (pos != val_str.size() || val < 2) {
                    std::cerr << "Error: --sort-fan-in must be an integer >= 2 (got '" << val_str << "')\n";
                    return 2;
                }
                opt_sort_fan_in = static_cast<size_t>(val);
            } catch (...) {
                std::cerr << "Error: --sort-fan-in must be an integer >= 2 (got '" << val_str << "')\n";
                return 2;
            }
        } else {
            std::cerr << "Error: unrecognized argument '" << arg << "'\n";
            return 2;
        }
    }

    // ------------------------------------------------------------------------
    // Mode: SQL Query (--table t=data.csv --sql "SELECT ...")
    // ------------------------------------------------------------------------
    if (mode_sql) {
        if (table_arg.empty()) {
            std::cerr << "Error: missing --table argument for SQL query\n";
            return 2;
        }

        SelectStmt stmt;
        try {
            SqlParser parser(sql_query);
            stmt = parser.parseSelect();
        } catch (const ParseError& e) {
            std::cerr << e.what();
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "Parse error: " << e.what() << "\n";
            return 1;
        }

        PlanOptions plan_options;
        plan_options.max_groups = opt_max_groups;
        plan_options.memory_limit_mb = opt_memory_limit_mb;
        plan_options.tmp_dir = opt_tmp_dir;
        plan_options.sort_stats = opt_sort_stats;
        plan_options.sort_fan_in = opt_sort_fan_in;

        Plan plan = buildPlan(stmt, table_arg, plan_options);

        if (opt_explain) {
            plan.explain(stdout);
            return 0;
        }

        auto start_time = std::chrono::steady_clock::now();

        if (!plan.root->open()) {
            std::cerr << "Error: failed to open query operator\n";
            return 2;
        }

        Tuple tuple;
        long long row_count = 0;

        if (opt_no_output) {
            while (plan.root->next(tuple)) {
                row_count++;
            }
            plan.root->close();

            auto end_time = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
            std::fprintf(stderr, "%lld rows in %.4fs (%.1f rows/s)\n",
                         row_count, elapsed_sec,
                         elapsed_sec > 0.0 ? (row_count / elapsed_sec) : 0.0);
            return 0;
        }

        // Print header line
        for (size_t i = 0; i < plan.visible_width; ++i) {
            if (i > 0) std::putchar(',');
            printHeaderField(plan.output_headers[i]);
        }
        std::putchar('\n');

        // Print rows
        while (plan.root->next(tuple)) {
            for (size_t i = 0; i < plan.visible_width; ++i) {
                if (i > 0) std::putchar(',');
                formatValue(tuple.values[i], stdout);
            }
            std::putchar('\n');
            row_count++;
        }
        plan.root->close();
        return 0;
    }

    // ------------------------------------------------------------------------
    // Legacy Phase 1 modes (--file ...)
    // ------------------------------------------------------------------------
    if (file_path.empty()) {
        std::cerr << "Error: missing --file argument\n";
        return 2;
    }

    CsvScanner scanner;
    if (!scanner.open(file_path.c_str())) {
        std::cerr << "Error: cannot open or read file '" << file_path << "'\n";
        return 2;
    }

    // Mode: --schema
    if (mode_schema) {
        const auto& cols = scanner.columns();
        for (const auto& col : cols) {
            const char* type_str = "STRING";
            if (col.type == TYPE_INT) type_str = "INT";
            else if (col.type == TYPE_FLOAT) type_str = "FLOAT";
            std::cout << col.name << ": " << type_str << "\n";
        }
        return 0;
    }

    // Mode: --count
    if (mode_count) {
        auto start_time = std::chrono::steady_clock::now();
        Row row;
        long long count = 0;
        while (scanner.nextRow(row)) {
            count++;
        }
        auto end_time = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
        double mb = static_cast<double>(scanner.fileSize()) / (1024.0 * 1024.0);
        double mb_per_sec = elapsed_sec > 0.0 ? (mb / elapsed_sec) : 0.0;

        std::cout << "Rows: " << count << "\n";
        std::cout << "Scan speed: " << mb_per_sec << " MB/s (" << elapsed_sec << " s)\n";
        return 0;
    }

    // Mode: --select col1,col2,...
    if (mode_select) {
        std::vector<std::string> requested_cols;
        if (select_cols == "*") {
            for (const auto& col : scanner.columns()) {
                requested_cols.push_back(col.name);
            }
        } else {
            std::stringstream ss(select_cols);
            std::string col_name;
            while (std::getline(ss, col_name, ',')) {
                requested_cols.push_back(col_name);
            }
        }

        const auto& all_cols = scanner.columns();
        std::vector<int> col_indices;
        for (const auto& req : requested_cols) {
            int found_idx = -1;
            for (size_t i = 0; i < all_cols.size(); ++i) {
                if (all_cols[i].name == req) {
                    found_idx = static_cast<int>(i);
                    break;
                }
            }
            if (found_idx < 0) {
                std::cerr << "Error: unknown column '" << req << "'\n";
                return 2;
            }
            col_indices.push_back(found_idx);
        }

        // Print header line
        for (size_t i = 0; i < requested_cols.size(); ++i) {
            if (i > 0) std::putchar(',');
            printHeaderField(requested_cols[i]);
        }
        std::putchar('\n');

        Row row;
        while (scanner.nextRow(row)) {
            for (size_t i = 0; i < col_indices.size(); ++i) {
                if (i > 0) std::putchar(',');
                int idx = col_indices[i];
                if (idx < static_cast<int>(row.fields.size())) {
                    Value v = makeString(row.fields[idx]);
                    formatValue(v, stdout);
                }
            }
            std::putchar('\n');
        }
        return 0;
    }

    std::cerr << "Error: no valid mode specified\n";
    return 2;
}

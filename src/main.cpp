#include "csv_scan.h"
#include "sql_parser.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>

static std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return lower;
}

static void printCsvField(std::string_view field) {
    if (isNull(field)) {
        // Empty CSV field represents SQL NULL
        return;
    }

    bool needs_quotes = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        // Print raw text directly (preserves exact float representation without reformatting)
        std::cout << field;
    } else {
        std::cout << '"';
        for (char c : field) {
            if (c == '"') {
                std::cout << "\"\"";
            } else {
                std::cout << c;
            }
        }
        std::cout << '"';
    }
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

        std::string table_name = "t";
        std::string csv_path = table_arg;
        size_t eq_pos = table_arg.find('=');
        if (eq_pos != std::string::npos) {
            table_name = table_arg.substr(0, eq_pos);
            csv_path = table_arg.substr(eq_pos + 1);
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

        // Check for unsupported features (exit code 3)
        if (stmt.distinct) {
            std::cerr << "not implemented yet: DISTINCT\n";
            return 3;
        }
        if (!stmt.joins.empty()) {
            std::cerr << "not implemented yet: JOIN\n";
            return 3;
        }
        if (stmt.where_clause) {
            std::cerr << "not implemented yet: WHERE\n";
            return 3;
        }
        if (!stmt.group_by.empty()) {
            std::cerr << "not implemented yet: GROUP BY\n";
            return 3;
        }
        if (stmt.having) {
            std::cerr << "not implemented yet: HAVING\n";
            return 3;
        }
        if (!stmt.order_by.empty()) {
            std::cerr << "not implemented yet: ORDER BY\n";
            return 3;
        }

        // Check table name match (case-insensitive)
        std::string lower_expected_table = toLower(table_name);
        std::string lower_from_table = toLower(stmt.from_table.name);
        if (lower_from_table != lower_expected_table) {
            std::cerr << "Error: unknown table '" << stmt.from_table.name << "'\n";
            return 2;
        }

        // Check select items
        std::string lower_alias = toLower(stmt.from_table.alias);
        for (const auto& item : stmt.items) {
            if (!item.alias.empty()) {
                std::cerr << "not implemented yet: ALIAS\n";
                return 3;
            }
            if (item.expr->kind != ExprKind::COLUMN_REF && item.expr->kind != ExprKind::STAR) {
                std::cerr << "not implemented yet: EXPRESSION\n";
                return 3;
            }
            if (!item.expr->table_name.empty()) {
                std::string lower_prefix = toLower(item.expr->table_name);
                if (lower_prefix != lower_expected_table &&
                    (lower_alias.empty() || lower_prefix != lower_alias)) {
                    std::cerr << "Error: unknown table qualifier '" << item.expr->table_name << "'\n";
                    return 2;
                }
            }
        }

        // Open CSV file
        CsvScanner scanner;
        if (!scanner.open(csv_path.c_str())) {
            std::cerr << "Error: cannot open or read file '" << csv_path << "'\n";
            return 2;
        }

        const auto& all_cols = scanner.columns();
        std::vector<int> col_indices;

        for (const auto& item : stmt.items) {
            if (item.expr->kind == ExprKind::STAR) {
                for (size_t c = 0; c < all_cols.size(); ++c) {
                    col_indices.push_back(static_cast<int>(c));
                }
            } else {
                std::string target_col_lower = toLower(item.expr->col_name);
                int found_idx = -1;
                for (size_t c = 0; c < all_cols.size(); ++c) {
                    if (toLower(all_cols[c].name) == target_col_lower) {
                        found_idx = static_cast<int>(c);
                        break;
                    }
                }
                if (found_idx < 0) {
                    std::cerr << "Error: unknown column '" << item.expr->col_name << "'\n";
                    return 2;
                }
                col_indices.push_back(found_idx);
            }
        }

        // Print header line
        for (size_t i = 0; i < col_indices.size(); ++i) {
            if (i > 0) std::cout << ",";
            printCsvField(all_cols[col_indices[i]].name);
        }
        std::cout << "\n";

        // Handle OFFSET
        long long offset = stmt.offset.value_or(0);
        Row row;
        while (offset > 0 && scanner.nextRow(row)) {
            offset--;
        }

        // Handle LIMIT
        long long limit = stmt.limit.value_or(-1);
        long long rows_printed = 0;

        while ((limit < 0 || rows_printed < limit) && scanner.nextRow(row)) {
            for (size_t i = 0; i < col_indices.size(); ++i) {
                if (i > 0) std::cout << ",";
                int idx = col_indices[i];
                if (idx < static_cast<int>(row.fields.size())) {
                    printCsvField(row.fields[idx]);
                }
            }
            std::cout << "\n";
            rows_printed++;
        }
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

    if (mode_schema) {
        for (const auto& col : scanner.columns()) {
            std::string type_name = "STRING";
            if (col.type == TYPE_INT) type_name = "INT";
            else if (col.type == TYPE_FLOAT) type_name = "FLOAT";
            std::cout << col.name << "," << type_name << "\n";
        }
        return 0;
    }

    if (mode_count) {
        auto start_time = std::chrono::high_resolution_clock::now();
        Row row;
        long long row_count = 0;
        while (scanner.nextRow(row)) {
            row_count++;
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end_time - start_time;
        double elapsed_sec = diff.count();
        double mb = static_cast<double>(scanner.fileSize()) / (1024.0 * 1024.0);
        double mb_per_sec = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

        std::cerr << "Rows: " << row_count
                  << ", Elapsed: " << elapsed_sec << " s"
                  << ", Speed: " << mb_per_sec << " MB/s\n";
        return 0;
    }

    if (mode_select) {
        std::vector<int> col_indices;
        const auto& all_cols = scanner.columns();

        if (select_cols == "*") {
            for (size_t i = 0; i < all_cols.size(); ++i) {
                col_indices.push_back(static_cast<int>(i));
            }
        } else {
            std::stringstream ss(select_cols);
            std::string item;
            while (std::getline(ss, item, ',')) {
                size_t start = item.find_first_not_of(" \t");
                size_t end = item.find_last_not_of(" \t");
                if (start == std::string::npos) continue;
                std::string col_name = item.substr(start, end - start + 1);

                int idx = scanner.columnIndex(col_name);
                if (idx < 0) {
                    std::cerr << "Error: unknown column '" << col_name << "'\n";
                    return 2;
                }
                col_indices.push_back(idx);
            }
        }

        for (size_t i = 0; i < col_indices.size(); ++i) {
            if (i > 0) std::cout << ",";
            printCsvField(all_cols[col_indices[i]].name);
        }
        std::cout << "\n";

        Row row;
        while (scanner.nextRow(row)) {
            for (size_t i = 0; i < col_indices.size(); ++i) {
                if (i > 0) std::cout << ",";
                int idx = col_indices[i];
                if (idx < static_cast<int>(row.fields.size())) {
                    printCsvField(row.fields[idx]);
                }
            }
            std::cout << "\n";
        }
        return 0;
    }

    std::cerr << "Error: no operation specified (e.g. --select, --schema, --count, --sql)\n";
    return 2;
}

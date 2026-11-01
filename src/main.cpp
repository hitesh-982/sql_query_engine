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
#include <cstring>
#include <cstdio>

class FastOutputBuffer {
public:
    static constexpr size_t BUFFER_SIZE = 65536;
    FastOutputBuffer(FILE* out = stdout) : out_(out), pos_(0) {}
    ~FastOutputBuffer() { flush(); }

    void flush() {
        if (pos_ > 0) {
            std::fwrite(buffer_, 1, pos_, out_);
            pos_ = 0;
        }
    }

    inline void writeChar(char c) {
        if (pos_ >= BUFFER_SIZE) {
            flush();
        }
        buffer_[pos_++] = c;
    }

    inline void writeString(const char* data, size_t len) {
        while (len > 0) {
            if (pos_ == BUFFER_SIZE) {
                flush();
            }
            size_t avail = BUFFER_SIZE - pos_;
            size_t chunk = std::min(len, avail);
            std::memcpy(buffer_ + pos_, data, chunk);
            pos_ += chunk;
            data += chunk;
            len -= chunk;
        }
    }

    inline void writeStringView(std::string_view sv) {
        writeString(sv.data(), sv.size());
    }

    inline void writeInt(long long val) {
        if (BUFFER_SIZE - pos_ < 32) {
            flush();
        }
        pos_ += formatIntToBuffer(buffer_ + pos_, val);
    }

    void writeValue(Value v) {
        switch (v.type) {
            case V_NULL:
                return;
            case V_INT:
                writeInt(v.i);
                return;
            case V_FLOAT: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.15g", v.d);
                bool has_special = false;
                for (char* p = buf; *p; ++p) {
                    if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') {
                        has_special = true;
                        break;
                    }
                }
                writeString(buf, std::strlen(buf));
                if (!has_special) {
                    writeString(".0", 2);
                }
                return;
            }
            case V_STRING: {
                bool needs_quotes = false;
                for (char c : v.s) {
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                        needs_quotes = true;
                        break;
                    }
                }
                if (!needs_quotes) {
                    writeString(v.s.data(), v.s.size());
                } else {
                    writeChar('"');
                    for (char c : v.s) {
                        if (c == '"') {
                            writeString("\"\"", 2);
                        } else {
                            writeChar(c);
                        }
                    }
                    writeChar('"');
                }
                return;
            }
        }
    }

private:
    static size_t formatIntToBuffer(char* dst, long long val) {
        if (val == 0) {
            dst[0] = '0';
            return 1;
        }
        char* p = dst;
        unsigned long long u = static_cast<unsigned long long>(val);
        if (val < 0) {
            *p++ = '-';
            u = 0ULL - u;
        }
        char temp[24];
        int idx = 0;
        while (u > 0) {
            temp[idx++] = static_cast<char>('0' + (u % 10));
            u /= 10;
        }
        while (idx > 0) {
            *p++ = temp[--idx];
        }
        return static_cast<size_t>(p - dst);
    }

    FILE* out_;
    size_t pos_;
    char buffer_[BUFFER_SIZE];
};



// Executes a built query plan tuple-by-tuple, printing CSV results or timing
static int executeQuery(const SelectStmt& stmt,
                        const std::vector<std::string>& table_args,
                        const PlanOptions& plan_options,
                        bool opt_explain,
                        bool opt_no_output) {
    Plan plan = buildPlan(stmt, table_args, plan_options);

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

    FastOutputBuffer out_buf(stdout);

    // Print header line
    for (size_t i = 0; i < plan.visible_width; ++i) {
        if (i > 0) out_buf.writeChar(',');
        out_buf.writeValue(makeString(plan.output_headers[i]));
    }
    out_buf.writeChar('\n');

    // Print rows
    while (plan.root->next(tuple)) {
        for (size_t i = 0; i < plan.visible_width; ++i) {
            if (i > 0) out_buf.writeChar(',');
            out_buf.writeValue(tuple.values[i]);
        }
        out_buf.writeChar('\n');
        row_count++;
    }
    out_buf.flush();
    plan.root->close();
    return 0;
}

int main(int argc, char* argv[]) {
    std::string file_path;
    std::string select_cols;
    std::vector<std::string> table_args;
    std::string sql_query;
    bool mode_select = false;
    bool mode_schema = false;
    bool mode_count = false;
    bool mode_sql = false;
    bool mode_interactive = false;
    bool opt_explain = false;
    bool opt_no_output = false;
    bool opt_no_pushdown = false;
    bool opt_no_join_swap = false;
    bool opt_join_stats = false;
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
        if (arg == "--help" || arg == "-h") {
            std::cout << "SQL Query Engine v1.0 (Phase 8)\n\n"
                      << "Usage:\n"
                      << "  ./engine --table <name>=<file.csv> ... --sql \"<query>\"   Execute SQL query\n"
                      << "  ./engine --table <name>=<file.csv> ... --interactive     Interactive SQL shell\n"
                      << "  ./engine --web [port]                                   Launch Web UI Studio (default port 8080)\n"
                      << "  ./engine --file <file.csv> --schema                     Inspect inferred column types\n"
                      << "  ./engine --file <file.csv> --count                      Benchmark CSV scan throughput\n"
                      << "  ./engine --file <file.csv> --select col1,col2           Filter columns\n\n"
                      << "Options:\n"
                      << "  --explain            Print physical operator plan tree\n"
                      << "  --memory-limit-mb N  Set external sort memory threshold (MB)\n"
                      << "  --no-pushdown        Disable filter pushdown optimization\n"
                      << "  --no-join-swap       Disable hash join probe/build swap\n"
                      << "  --sort-stats         Print external merge sort telemetry\n"
                      << "  --join-stats         Print hash join execution telemetry\n";
            return 0;
        } else if (arg == "--web" || arg == "--serve") {
            int port = 8080;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { port = std::stoi(argv[++i]); } catch (...) {}
            }
            std::string cmd = "python3 tools/side_by_side_app.py " + std::to_string(port);
            return std::system(cmd.c_str());
        } else if (arg == "--interactive" || arg == "-i") {
            mode_interactive = true;
        } else if (arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "--select" && i + 1 < argc) {
            mode_select = true;
            select_cols = argv[++i];
        } else if (arg == "--schema") {
            mode_schema = true;
        } else if (arg == "--count") {
            mode_count = true;
        } else if (arg == "--table" && i + 1 < argc) {
            table_args.push_back(argv[++i]);
        } else if (arg == "--sql" && i + 1 < argc) {
            mode_sql = true;
            sql_query = argv[++i];
        } else if (arg == "--explain") {
            opt_explain = true;
        } else if (arg == "--no-output") {
            opt_no_output = true;
        } else if (arg == "--no-pushdown") {
            opt_no_pushdown = true;
        } else if (arg == "--no-join-swap") {
            opt_no_join_swap = true;
        } else if (arg == "--join-stats") {
            opt_join_stats = true;
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
        if (table_args.empty()) {
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
        plan_options.no_pushdown = opt_no_pushdown;
        plan_options.no_join_swap = opt_no_join_swap;
        plan_options.join_stats = opt_join_stats;

        return executeQuery(stmt, table_args, plan_options, opt_explain, opt_no_output);
    }

    // ------------------------------------------------------------------------
    // Mode: Interactive SQL REPL Shell
    // ------------------------------------------------------------------------
    if (mode_interactive || (!table_args.empty() && !mode_sql && file_path.empty() && !mode_schema && !mode_count && !mode_select)) {
        std::cout << "============================================================\n";
        std::cout << "  SQL Query Engine v1.0 - Interactive Shell\n";
        std::cout << "  Tables loaded: " << table_args.size() << "\n";
        for (const auto& t : table_args) {
            std::cout << "    - " << t << "\n";
        }
        std::cout << "  Type SQL query ending with ';' | Type 'exit;' to quit.\n";
        std::cout << "============================================================\n";

        PlanOptions plan_options;
        plan_options.max_groups = opt_max_groups;
        plan_options.memory_limit_mb = opt_memory_limit_mb;
        plan_options.tmp_dir = opt_tmp_dir;
        plan_options.sort_stats = opt_sort_stats;
        plan_options.sort_fan_in = opt_sort_fan_in;
        plan_options.no_pushdown = opt_no_pushdown;
        plan_options.no_join_swap = opt_no_join_swap;
        plan_options.join_stats = opt_join_stats;

        std::string buffer;
        std::string line;
        while (true) {
            if (buffer.empty()) {
                std::cout << "sql> " << std::flush;
            } else {
                std::cout << " ... " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                break;
            }
            std::string trimmed = line;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();

            if (trimmed == "exit;" || trimmed == "quit;" || trimmed == "exit" || trimmed == "quit") {
                break;
            }

            buffer += " " + line;
            size_t semi = buffer.find(';');
            if (semi != std::string::npos) {
                std::string q = buffer.substr(0, semi);
                buffer = buffer.substr(semi + 1);

                try {
                    SqlParser parser(q);
                    SelectStmt stmt = parser.parseSelect();
                    auto t0 = std::chrono::steady_clock::now();
                    executeQuery(stmt, table_args, plan_options, opt_explain, false);
                    auto t1 = std::chrono::steady_clock::now();
                    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    std::cout << "(" << ms << " ms)\n\n";
                } catch (const ParseError& e) {
                    std::cerr << e.what() << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << "\n";
                }
            }
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

        FastOutputBuffer out_buf(stdout);

        // Print header line
        for (size_t i = 0; i < requested_cols.size(); ++i) {
            if (i > 0) out_buf.writeChar(',');
            out_buf.writeValue(makeString(requested_cols[i]));
        }
        out_buf.writeChar('\n');

        Row row;
        while (scanner.nextRow(row)) {
            for (size_t i = 0; i < col_indices.size(); ++i) {
                if (i > 0) out_buf.writeChar(',');
                int idx = col_indices[i];
                if (idx < static_cast<int>(row.fields.size())) {
                    out_buf.writeValue(makeString(row.fields[idx]));
                }
            }
            out_buf.writeChar('\n');
        }
        out_buf.flush();
        return 0;
    }

    std::cerr << "Error: no valid mode specified\n";
    return 2;
}

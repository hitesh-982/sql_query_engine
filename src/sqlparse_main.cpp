#include "sql_parser.h"

#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [--to-sql|--roundtrip] [\"SELECT ...\" | -f file.sql]\n";
        return 1;
    }

    bool mode_to_sql = false;
    bool mode_roundtrip = false;
    int arg_idx = 1;

    if (std::string(argv[arg_idx]) == "--to-sql") {
        mode_to_sql = true;
        arg_idx++;
    } else if (std::string(argv[arg_idx]) == "--roundtrip") {
        mode_roundtrip = true;
        arg_idx++;
    }

    if (arg_idx >= argc) {
        std::cerr << "Error: missing SQL input\n";
        return 1;
    }

    std::string sql;
    std::string current_arg = argv[arg_idx];

    if (current_arg == "-f") {
        if (arg_idx + 1 >= argc) {
            std::cerr << "Error: missing file argument after -f\n";
            return 1;
        }
        std::ifstream file(argv[arg_idx + 1]);
        if (!file.is_open()) {
            std::cerr << "Error: cannot open SQL file '" << argv[arg_idx + 1] << "'\n";
            return 1;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        sql = buffer.str();
    } else {
        sql = current_arg;
    }

    try {
        SqlParser parser1(sql);
        SelectStmt stmt1 = parser1.parseSelect();

        if (mode_to_sql) {
            std::cout << stmt1.toSql() << "\n";
            return 0;
        }

        if (mode_roundtrip) {
            std::string ast1 = stmt1.toSExpr();
            std::string canonical_sql = stmt1.toSql();

            SqlParser parser2(canonical_sql);
            SelectStmt stmt2 = parser2.parseSelect();
            std::string ast2 = stmt2.toSExpr();

            if (ast1 != ast2) {
                std::cerr << "Round-trip AST mismatch!\n";
                std::cerr << "Original SQL:  " << sql << "\n";
                std::cerr << "Canonical SQL: " << canonical_sql << "\n";
                std::cerr << "AST 1:\n" << ast1 << "\n";
                std::cerr << "AST 2:\n" << ast2 << "\n";
                return 1;
            }
            std::cout << ast1 << "\n";
            return 0;
        }

        // Default mode: print S-expression
        std::cout << stmt1.toSExpr() << "\n";
        return 0;
    } catch (const ParseError& e) {
        std::cerr << e.what();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

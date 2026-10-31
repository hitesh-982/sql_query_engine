#ifndef OPS_BASIC_H
#define OPS_BASIC_H

#include "operator.h"
#include "csv_scan.h"
#include "expr_eval.h"

#include <vector>
#include <string>
#include <memory>

// ScanOperator: wraps CsvScanner, converts only needed columns into Values
class ScanOperator : public Operator {
private:
    std::string file_path_;
    std::vector<size_t> needed_cols_;
    std::vector<ColType> col_types_;
    std::vector<std::string> col_names_;
    CsvScanner scanner_;
    Row current_row_;

public:
    ScanOperator(
        std::string file_path,
        std::vector<size_t> needed_cols,
        std::vector<ColType> col_types,
        std::vector<std::string> col_names
    );
    ~ScanOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;
    const std::vector<std::string>& columnNames() const override;
    const std::vector<size_t>& neededColumns() const { return needed_cols_; }
    const std::string& filePath() const { return file_path_; }
};

// FilterOperator: filters rows based on a WHERE BoundExpr
class FilterOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    std::unique_ptr<BoundExpr> condition_;

public:
    FilterOperator(std::unique_ptr<Operator> child, std::unique_ptr<BoundExpr> condition);
    ~FilterOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;
    const std::vector<std::string>& columnNames() const override;
    const BoundExpr* condition() const { return condition_.get(); }
    const Operator* child() const { return child_.get(); }
};

// ProjectOperator: projects expressions to construct output tuples
class ProjectOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    std::vector<std::unique_ptr<BoundExpr>> exprs_;
    std::vector<std::string> output_col_names_;
    Tuple child_tuple_;

public:
    ProjectOperator(
        std::unique_ptr<Operator> child,
        std::vector<std::unique_ptr<BoundExpr>> exprs,
        std::vector<std::string> output_col_names
    );
    ~ProjectOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;
    const std::vector<std::string>& columnNames() const override;
    const std::vector<std::unique_ptr<BoundExpr>>& exprs() const { return exprs_; }
    const Operator* child() const { return child_.get(); }
};

// LimitOperator: implements OFFSET and LIMIT with early termination
class LimitOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    long long limit_;   // -1 means no limit
    long long offset_;  // 0 means no offset
    long long yielded_ = 0;

public:
    LimitOperator(std::unique_ptr<Operator> child, long long limit, long long offset);
    ~LimitOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;
    const std::vector<std::string>& columnNames() const override;
    long long limit() const { return limit_; }
    long long offset() const { return offset_; }
    const Operator* child() const { return child_.get(); }
};

#endif // OPS_BASIC_H

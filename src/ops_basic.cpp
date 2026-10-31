#include "ops_basic.h"

// -----------------------------------------------------------------------------
// ScanOperator
// -----------------------------------------------------------------------------

ScanOperator::ScanOperator(
    std::string file_path,
    std::vector<size_t> needed_cols,
    std::vector<ColType> col_types,
    std::vector<std::string> col_names
)
    : file_path_(std::move(file_path)),
      needed_cols_(std::move(needed_cols)),
      col_types_(std::move(col_types)),
      col_names_(std::move(col_names)) {}

ScanOperator::~ScanOperator() {
    close();
}

bool ScanOperator::open() {
    return scanner_.open(file_path_.c_str());
}

bool ScanOperator::next(Tuple& out) {
    if (!scanner_.nextRow(current_row_)) {
        return false;
    }
    if (out.values.size() != col_names_.size()) {
        out.values.resize(col_names_.size(), makeNull());
    }
    // Only convert fields requested by the query plan
    for (size_t col_idx : needed_cols_) {
        if (col_idx < current_row_.fields.size()) {
            out.values[col_idx] = valueFromCsvField(current_row_.fields[col_idx], col_types_[col_idx]);
        } else {
            out.values[col_idx] = makeNull();
        }
    }
    return true;
}

void ScanOperator::close() {
    scanner_.close();
}

const std::vector<std::string>& ScanOperator::columnNames() const {
    return col_names_;
}

// -----------------------------------------------------------------------------
// FilterOperator
// -----------------------------------------------------------------------------

FilterOperator::FilterOperator(
    std::unique_ptr<Operator> child,
    std::unique_ptr<BoundExpr> condition
)
    : child_(std::move(child)), condition_(std::move(condition)) {}

FilterOperator::~FilterOperator() {
    close();
}

bool FilterOperator::open() {
    return child_ ? child_->open() : false;
}

bool FilterOperator::next(Tuple& out) {
    while (child_->next(out)) {
        Value res = evalExpr(condition_.get(), out);
        if (isTruthy(res)) {
            return true;
        }
    }
    return false;
}

void FilterOperator::close() {
    if (child_) child_->close();
}

const std::vector<std::string>& FilterOperator::columnNames() const {
    return child_->columnNames();
}

// -----------------------------------------------------------------------------
// ProjectOperator
// -----------------------------------------------------------------------------

ProjectOperator::ProjectOperator(
    std::unique_ptr<Operator> child,
    std::vector<std::unique_ptr<BoundExpr>> exprs,
    std::vector<std::string> output_col_names
)
    : child_(std::move(child)),
      exprs_(std::move(exprs)),
      output_col_names_(std::move(output_col_names)) {}

ProjectOperator::~ProjectOperator() {
    close();
}

bool ProjectOperator::open() {
    if (!child_ || !child_->open()) return false;
    child_tuple_.values.resize(child_->columnNames().size(), makeNull());
    return true;
}

bool ProjectOperator::next(Tuple& out) {
    if (!child_->next(child_tuple_)) {
        return false;
    }
    if (out.values.size() != exprs_.size()) {
        out.values.resize(exprs_.size(), makeNull());
    }
    for (size_t i = 0; i < exprs_.size(); ++i) {
        out.values[i] = evalExpr(exprs_[i].get(), child_tuple_);
    }
    return true;
}

void ProjectOperator::close() {
    if (child_) child_->close();
}

const std::vector<std::string>& ProjectOperator::columnNames() const {
    return output_col_names_;
}

// -----------------------------------------------------------------------------
// LimitOperator
// -----------------------------------------------------------------------------

LimitOperator::LimitOperator(
    std::unique_ptr<Operator> child,
    long long limit,
    long long offset
)
    : child_(std::move(child)), limit_(limit), offset_(offset), yielded_(0) {}

LimitOperator::~LimitOperator() {
    close();
}

bool LimitOperator::open() {
    if (!child_ || !child_->open()) return false;
    yielded_ = 0;

    // LIMIT 0 must not pull any rows, even if OFFSET is specified
    if (limit_ == 0) {
        return true;
    }

    // Skip offset rows
    long long to_skip = offset_;
    Tuple dummy;
    dummy.values.resize(child_->columnNames().size(), makeNull());
    while (to_skip > 0 && child_->next(dummy)) {
        to_skip--;
    }
    return true;
}

bool LimitOperator::next(Tuple& out) {
    if (limit_ >= 0 && yielded_ >= limit_) {
        // Once limit is reached, stop pulling from child so scan terminates early
        return false;
    }
    if (!child_->next(out)) {
        return false;
    }
    yielded_++;
    return true;
}

void LimitOperator::close() {
    if (child_) child_->close();
}

const std::vector<std::string>& LimitOperator::columnNames() const {
    return child_->columnNames();
}

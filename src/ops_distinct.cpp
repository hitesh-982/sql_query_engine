#include "ops_distinct.h"

#include <iostream>
#include <cstdlib>

DistinctOperator::DistinctOperator(
    std::unique_ptr<Operator> child,
    size_t visible_width,
    size_t max_groups
)
    : child_(std::move(child)),
      visible_width_(visible_width),
      max_groups_(max_groups) {}

DistinctOperator::~DistinctOperator() {
    close();
}

bool DistinctOperator::open() {
    seen_keys_.clear();
    key_buf_.clear();
    return child_ ? child_->open() : false;
}

bool DistinctOperator::next(Tuple& out) {
    while (child_->next(out)) {
        size_t count = visible_width_;
        if (count > out.values.size()) {
            count = out.values.size();
        }
        encodeGroupKey(out.values.data(), count, key_buf_);
        if (seen_keys_.find(key_buf_) == seen_keys_.end()) {
            if (max_groups_ > 0 && seen_keys_.size() >= max_groups_) {
                std::cerr << "Error: maximum number of groups exceeded (" << max_groups_ << ")\n";
                std::exit(4);
            }
            try {
                seen_keys_.insert(key_buf_);
            } catch (const std::bad_alloc&) {
                std::cerr << "Error: out of memory in DISTINCT (" << seen_keys_.size() << " distinct rows so far)\n";
                std::exit(4);
            }
            return true;
        }
    }
    return false;
}

void DistinctOperator::close() {
    seen_keys_.clear();
    key_buf_.clear();
    if (child_) {
        child_->close();
    }
}

const std::vector<std::string>& DistinctOperator::columnNames() const {
    return child_->columnNames();
}


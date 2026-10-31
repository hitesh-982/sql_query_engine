#ifndef OPS_DISTINCT_H
#define OPS_DISTINCT_H

#include "operator.h"
#include "group_key.h"

#include <memory>
#include <unordered_set>
#include <string>
#include <vector>

class DistinctOperator : public Operator {
public:
    DistinctOperator(
        std::unique_ptr<Operator> child,
        size_t visible_width,
        size_t max_groups = 0
    );
    ~DistinctOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;

    const std::vector<std::string>& columnNames() const override;

private:
    std::unique_ptr<Operator> child_;
    size_t visible_width_;
    size_t max_groups_;

    std::unordered_set<std::string> seen_keys_;
    std::string key_buf_;
};

#endif // OPS_DISTINCT_H


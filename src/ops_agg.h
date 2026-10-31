#ifndef OPS_AGG_H
#define OPS_AGG_H

#include "operator.h"
#include "expr_eval.h"
#include "value.h"

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <cmath>

enum class AggFunc {
    COUNT_STAR,
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX
};

struct AggSpec {
    AggFunc func;
    std::unique_ptr<BoundExpr> arg; // nullptr for COUNT_STAR
    std::string sql;                // For explain and debugging
};

struct AggState {
    long long count = 0; // Number of non-null rows (or all rows for COUNT_STAR)

    // For SUM / AVG:
    bool sum_is_float = false;
    long long sum_i = 0;
    bool sum_overflow = false;
    double sum_d = 0.0;
    double sum_c = 0.0; // Neumaier compensation term

    // For MIN / MAX:
    bool has_val = false;
    Value min_max_val = makeNull();
};

class AggregateOperator : public Operator {
public:
    AggregateOperator(
        std::unique_ptr<Operator> child,
        std::vector<std::unique_ptr<BoundExpr>> group_by_exprs,
        std::vector<AggSpec> agg_specs,
        size_t max_groups = 0
    );
    ~AggregateOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;

    const std::vector<std::string>& columnNames() const override;

    // Helper for encoding composite key into a buffer
    static void encodeKey(const std::vector<Value>& values, std::string& buf);

    // Helpers for updating aggregate state
    static void addFloatToSum(AggState& st, double val);
    static void addIntToSum(AggState& st, long long val);

private:
    std::unique_ptr<Operator> child_;
    std::vector<std::unique_ptr<BoundExpr>> group_by_exprs_;
    std::vector<AggSpec> agg_specs_;
    size_t max_groups_;

    std::vector<std::string> output_col_names_;
    std::vector<Tuple> finalized_tuples_;
    size_t cursor_ = 0;
};

#endif // OPS_AGG_H

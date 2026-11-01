#ifndef OPS_JOIN_H
#define OPS_JOIN_H

#include "operator.h"
#include "sql_ast.h"
#include "expr_eval.h"
#include "group_key.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

class HashJoinOperator : public Operator {
private:
    std::unique_ptr<Operator> probe_child_;
    std::unique_ptr<Operator> build_child_;
    JoinType join_type_;
    bool swapped_; // true if build side is written left (swapped INNER joins)
    std::vector<std::unique_ptr<BoundExpr>> probe_keys_;
    std::vector<std::unique_ptr<BoundExpr>> build_keys_;
    std::vector<Affinity> probe_key_affinities_;
    std::vector<Affinity> build_key_affinities_;
    std::unique_ptr<BoundExpr> residual_predicate_; // bound against combined tuple
    size_t probe_width_;
    size_t build_width_;
    std::vector<size_t> kept_build_slots_;
    size_t written_left_width_;
    size_t written_right_width_;
    bool is_nested_loop_;
    bool join_stats_;

    std::vector<std::string> column_names_;
    size_t total_width_;

    // Build side storage
    size_t build_row_count_ = 0;
    std::vector<Value> build_store_; // flat storage for kept build columns (build_row_count * kept_build_slots.size())
    std::vector<size_t> next_row_;   // duplicate chaining in build order
    std::unordered_map<std::string, std::pair<size_t, size_t>> hash_table_; // key -> {first_row, last_row}

    // Reused buffers
    std::string key_buf_;
    std::vector<Value> key_copies_;
    Tuple probe_tuple_buf_;
    Tuple current_probe_tuple_; // owned copy of current probe row
    Tuple combined_tuple_;

    // Resumable state for next()
    size_t current_build_idx_ = static_cast<size_t>(-1);
    bool probe_matched_ = false;

    // Statistics
    size_t probe_rows_read_ = 0;
    size_t rows_output_ = 0;

    void assembleCombinedTuple(const Tuple& probe_tuple, size_t build_idx, Tuple& out);
    void emitNullExtended(const Tuple& probe_tuple, Tuple& out);

public:
    HashJoinOperator(
        std::unique_ptr<Operator> probe_child,
        std::unique_ptr<Operator> build_child,
        JoinType join_type,
        bool swapped,
        std::vector<std::unique_ptr<BoundExpr>> probe_keys,
        std::vector<std::unique_ptr<BoundExpr>> build_keys,
        std::vector<Affinity> probe_key_affinities,
        std::vector<Affinity> build_key_affinities,
        std::unique_ptr<BoundExpr> residual_predicate,
        size_t probe_width,
        size_t build_width,
        std::vector<size_t> kept_build_slots,
        size_t written_left_width,
        size_t written_right_width,
        bool is_nested_loop,
        bool join_stats
    );
    ~HashJoinOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;
    const std::vector<std::string>& columnNames() const override;

    size_t buildRowCount() const { return build_row_count_; }
};

#endif // OPS_JOIN_H


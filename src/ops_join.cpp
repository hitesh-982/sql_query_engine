#include "ops_join.h"

#include <iostream>
#include <cstdio>
#include <cstdlib>

HashJoinOperator::HashJoinOperator(
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
)
    : probe_child_(std::move(probe_child)),
      build_child_(std::move(build_child)),
      join_type_(join_type),
      swapped_(swapped),
      probe_keys_(std::move(probe_keys)),
      build_keys_(std::move(build_keys)),
      probe_key_affinities_(std::move(probe_key_affinities)),
      build_key_affinities_(std::move(build_key_affinities)),
      residual_predicate_(std::move(residual_predicate)),
      probe_width_(probe_width),
      build_width_(build_width),
      kept_build_slots_(std::move(kept_build_slots)),
      written_left_width_(written_left_width),
      written_right_width_(written_right_width),
      is_nested_loop_(is_nested_loop),
      join_stats_(join_stats) {

    total_width_ = written_left_width_ + written_right_width_;
    combined_tuple_.values.resize(total_width_, makeNull());

    // Construct output column names in [written_left][written_right] order
    if (!swapped_) {
        for (const auto& name : probe_child_->columnNames()) {
            column_names_.push_back(name);
        }
        for (const auto& name : build_child_->columnNames()) {
            column_names_.push_back(name);
        }
    } else {
        for (const auto& name : build_child_->columnNames()) {
            column_names_.push_back(name);
        }
        for (const auto& name : probe_child_->columnNames()) {
            column_names_.push_back(name);
        }
    }
}

HashJoinOperator::~HashJoinOperator() {
    close();
}

bool HashJoinOperator::open() {
    if (!build_child_->open()) {
        return false;
    }
    if (!probe_child_->open()) {
        build_child_->close();
        return false;
    }

    build_store_.clear();
    next_row_.clear();
    hash_table_.clear();
    build_row_count_ = 0;
    probe_rows_read_ = 0;
    rows_output_ = 0;
    current_build_idx_ = static_cast<size_t>(-1);
    probe_matched_ = false;

    // Build phase: drain build_child
    try {
        Tuple b_tuple;
        while (build_child_->next(b_tuple)) {
            bool has_null_key = false;

            if (!is_nested_loop_) {
                // Evaluate build keys
                key_copies_.resize(build_keys_.size());
                for (size_t k = 0; k < build_keys_.size(); ++k) {
                    key_copies_[k] = evalExpr(build_keys_[k].get(), b_tuple);
                    if (key_copies_[k].type == V_NULL) {
                        has_null_key = true;
                        break;
                    }
                    // Apply SQLite comparison affinity normalization to a copy of the key value
                    char text_buf[64];
                    key_copies_[k] = normalizeKeyForAffinity(
                        key_copies_[k],
                        build_key_affinities_[k],
                        probe_key_affinities_[k],
                        text_buf,
                        sizeof(text_buf)
                    );
                }

                // In SQL, NULL keys never match any value (not even NULL)
                if (has_null_key) {
                    continue;
                }

                encodeGroupKey(key_copies_.data(), key_copies_.size(), key_buf_);
            }

            size_t row_idx = build_row_count_;
            build_row_count_++;

            // Store kept build columns
            for (size_t slot : kept_build_slots_) {
                if (slot < b_tuple.values.size()) {
                    build_store_.push_back(b_tuple.values[slot]);
                } else {
                    build_store_.push_back(makeNull());
                }
            }

            if (!is_nested_loop_) {
                auto it = hash_table_.find(key_buf_);
                if (it == hash_table_.end()) {
                    hash_table_.emplace(key_buf_, std::make_pair(row_idx, row_idx));
                    next_row_.push_back(static_cast<size_t>(-1));
                } else {
                    size_t prev_tail = it->second.second;
                    next_row_[prev_tail] = row_idx;
                    it->second.second = row_idx;
                    next_row_.push_back(static_cast<size_t>(-1));
                }
            }
        }
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "out of memory while building join hash table (%zu build rows)\n", build_row_count_);
        std::exit(4);
    }

    return true;
}

void HashJoinOperator::assembleCombinedTuple(const Tuple& probe_tuple, size_t build_idx, Tuple& out) {
    if (out.values.size() != total_width_) {
        out.values.resize(total_width_, makeNull());
    } else {
        std::fill(out.values.begin(), out.values.end(), makeNull());
    }

    size_t num_kept = kept_build_slots_.size();
    size_t build_store_offset = build_idx * num_kept;

    if (!swapped_) {
        // [probe (written-left)][build (written-right)]
        for (size_t i = 0; i < probe_width_ && i < probe_tuple.values.size(); ++i) {
            out.values[i] = probe_tuple.values[i];
        }
        for (size_t s = 0; s < num_kept; ++s) {
            size_t build_col_idx = kept_build_slots_[s];
            out.values[probe_width_ + build_col_idx] = build_store_[build_store_offset + s];
        }
    } else {
        // [build (written-left)][probe (written-right)]
        for (size_t s = 0; s < num_kept; ++s) {
            size_t build_col_idx = kept_build_slots_[s];
            out.values[build_col_idx] = build_store_[build_store_offset + s];
        }
        for (size_t i = 0; i < probe_width_ && i < probe_tuple.values.size(); ++i) {
            out.values[written_left_width_ + i] = probe_tuple.values[i];
        }
    }
}

void HashJoinOperator::emitNullExtended(const Tuple& probe_tuple, Tuple& out) {
    if (out.values.size() != total_width_) {
        out.values.resize(total_width_, makeNull());
    } else {
        std::fill(out.values.begin(), out.values.end(), makeNull());
    }

    // LEFT joins are never swapped, so probe is always written-left
    for (size_t i = 0; i < probe_width_ && i < probe_tuple.values.size(); ++i) {
        out.values[i] = probe_tuple.values[i];
    }
    // Build slots remain V_NULL
}

bool HashJoinOperator::next(Tuple& out) {
    while (true) {
        if (current_build_idx_ == static_cast<size_t>(-1)) {
            // Fetch next probe row
            if (!probe_child_->next(probe_tuple_buf_)) {
                return false;
            }
            probe_rows_read_++;
            // The probe state must own a copy of the current probe tuple
            current_probe_tuple_ = probe_tuple_buf_;
            probe_matched_ = false;

            if (is_nested_loop_) {
                if (build_row_count_ > 0) {
                    current_build_idx_ = 0;
                } else {
                    current_build_idx_ = static_cast<size_t>(-1);
                    if (join_type_ == JoinType::LEFT) {
                        emitNullExtended(current_probe_tuple_, out);
                        rows_output_++;
                        return true;
                    }
                    continue;
                }
            } else {
                // Hash join: evaluate probe keys
                bool has_null_key = false;
                key_copies_.resize(probe_keys_.size());
                for (size_t k = 0; k < probe_keys_.size(); ++k) {
                    key_copies_[k] = evalExpr(probe_keys_[k].get(), current_probe_tuple_);
                    if (key_copies_[k].type == V_NULL) {
                        has_null_key = true;
                        break;
                    }
                    char text_buf[64];
                    key_copies_[k] = normalizeKeyForAffinity(
                        key_copies_[k],
                        probe_key_affinities_[k],
                        build_key_affinities_[k],
                        text_buf,
                        sizeof(text_buf)
                    );
                }

                if (has_null_key) {
                    if (join_type_ == JoinType::LEFT) {
                        emitNullExtended(current_probe_tuple_, out);
                        rows_output_++;
                        return true;
                    }
                    continue; // Null keys never match in INNER join
                }

                encodeGroupKey(key_copies_.data(), key_copies_.size(), key_buf_);
                auto it = hash_table_.find(key_buf_);
                if (it == hash_table_.end()) {
                    if (join_type_ == JoinType::LEFT) {
                        emitNullExtended(current_probe_tuple_, out);
                        rows_output_++;
                        return true;
                    }
                    continue; // No key match in INNER join
                }
                current_build_idx_ = it->second.first; // head of chain
            }
        }

        // Walk candidate build rows for the current probe row
        while (current_build_idx_ != static_cast<size_t>(-1)) {
            size_t b_idx = current_build_idx_;

            // Advance current_build_idx_ for the subsequent call
            if (is_nested_loop_) {
                current_build_idx_++;
                if (current_build_idx_ >= build_row_count_) {
                    current_build_idx_ = static_cast<size_t>(-1);
                }
            } else {
                current_build_idx_ = next_row_[b_idx];
            }

            assembleCombinedTuple(current_probe_tuple_, b_idx, combined_tuple_);

            // Evaluate residual predicate
            if (residual_predicate_) {
                Value res = evalExpr(residual_predicate_.get(), combined_tuple_);
                if (res.type == V_NULL || !isTruthy(res)) {
                    continue; // residual condition failed
                }
            }

            probe_matched_ = true;
            out = combined_tuple_;
            rows_output_++;
            return true;
        }

        // Reached end of candidate build rows for this probe row
        if (!probe_matched_ && join_type_ == JoinType::LEFT) {
            emitNullExtended(current_probe_tuple_, out);
            rows_output_++;
            return true;
        }
    }
}

void HashJoinOperator::close() {
    if (join_stats_) {
        std::fprintf(stderr, "join: build rows stored %zu, probe rows read %zu, rows output %zu\n",
                     build_row_count_, probe_rows_read_, rows_output_);
        join_stats_ = false; // avoid printing twice if close() called multiple times
    }

    if (probe_child_) {
        probe_child_->close();
    }
    if (build_child_) {
        build_child_->close();
    }

    build_store_.clear();
    next_row_.clear();
    hash_table_.clear();
    build_row_count_ = 0;
    current_build_idx_ = static_cast<size_t>(-1);
}

const std::vector<std::string>& HashJoinOperator::columnNames() const {
    return column_names_;
}


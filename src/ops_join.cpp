#include "ops_join.h"

#include <iostream>
#include <cstdio>
#include <cstdlib>

HashJoinOperator::HashJoinOperator(
    std::unique_ptr<Operator> probe_child,
    std::unique_ptr<Operator> build_child,
    JoinType join_type,
    bool swapped,
    bool emit_unmatched_probe,
    bool emit_unmatched_build,
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
      emit_unmatched_probe_(emit_unmatched_probe),
      emit_unmatched_build_(emit_unmatched_build),
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
    : HashJoinOperator(
          std::move(probe_child),
          std::move(build_child),
          join_type,
          swapped,
          /*emit_unmatched_probe=*/(join_type == JoinType::LEFT),
          /*emit_unmatched_build=*/false,
          std::move(probe_keys),
          std::move(build_keys),
          std::move(probe_key_affinities),
          std::move(build_key_affinities),
          std::move(residual_predicate),
          probe_width,
          build_width,
          std::move(kept_build_slots),
          written_left_width,
          written_right_width,
          is_nested_loop,
          join_stats
      ) {}

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
    build_matched_.clear();
    hash_table_.clear();
    build_row_count_ = 0;
    probe_rows_read_ = 0;
    rows_output_ = 0;
    unmatched_probe_emitted_ = 0;
    unmatched_build_emitted_ = 0;
    unmatched_build_cursor_ = 0;
    current_build_idx_ = NO_ROW;
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

                // In SQL, NULL keys never match any value.
                // If emit_unmatched_build_ is false (INNER/LEFT), skip row completely.
                // If emit_unmatched_build_ is true (RIGHT/FULL), row must be stored but NOT indexed in hash table.
                if (has_null_key) {
                    if (!emit_unmatched_build_) {
                        continue;
                    }
                    // Store kept columns for NULL-key row (it remains unmatched forever)
                    for (size_t slot : kept_build_slots_) {
                        if (slot < b_tuple.values.size()) {
                            build_store_.push_back(b_tuple.values[slot]);
                        } else {
                            build_store_.push_back(makeNull());
                        }
                    }
                    next_row_.push_back(NO_ROW);
                    build_matched_.push_back(0);
                    build_row_count_++;
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

            if (emit_unmatched_build_) {
                build_matched_.push_back(0);
            }

            if (!is_nested_loop_) {
                auto it = hash_table_.find(key_buf_);
                if (it == hash_table_.end()) {
                    hash_table_.emplace(key_buf_, std::make_pair(row_idx, row_idx));
                    next_row_.push_back(NO_ROW);
                } else {
                    size_t prev_tail = it->second.second;
                    next_row_[prev_tail] = row_idx;
                    it->second.second = row_idx;
                    next_row_.push_back(NO_ROW);
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

    if (!swapped_) {
        // [probe (written-left)][build (written-right)]
        for (size_t i = 0; i < probe_width_ && i < probe_tuple.values.size(); ++i) {
            out.values[i] = probe_tuple.values[i];
        }
        // Build slots remain V_NULL
    } else {
        // [build (written-left)][probe (written-right)]
        // Build slots remain V_NULL
        for (size_t i = 0; i < probe_width_ && i < probe_tuple.values.size(); ++i) {
            out.values[written_left_width_ + i] = probe_tuple.values[i];
        }
    }
}

void HashJoinOperator::emitUnmatchedBuild(size_t build_idx, Tuple& out) {
    if (out.values.size() != total_width_) {
        out.values.resize(total_width_, makeNull());
    } else {
        std::fill(out.values.begin(), out.values.end(), makeNull());
    }

    size_t num_kept = kept_build_slots_.size();
    size_t build_store_offset = build_idx * num_kept;

    if (!swapped_) {
        // [probe (written-left)][build (written-right)]
        // Probe slots remain V_NULL
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
        // Probe slots remain V_NULL
    }
}

bool HashJoinOperator::next(Tuple& out) {
    while (true) {
        if (current_build_idx_ == NO_ROW) {
            // Fetch next probe row
            if (!probe_child_->next(probe_tuple_buf_)) {
                // Probe child exhausted. If emit_unmatched_build_ is enabled,
                // drain remaining unmatched build rows in build order.
                if (emit_unmatched_build_) {
                    while (unmatched_build_cursor_ < build_row_count_) {
                        size_t b_idx = unmatched_build_cursor_++;
                        if (build_matched_[b_idx] == 0) {
                            emitUnmatchedBuild(b_idx, out);
                            unmatched_build_emitted_++;
                            rows_output_++;
                            return true;
                        }
                    }
                }
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
                    current_build_idx_ = NO_ROW;
                    if (emit_unmatched_probe_) {
                        emitNullExtended(current_probe_tuple_, out);
                        unmatched_probe_emitted_++;
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
                    if (emit_unmatched_probe_) {
                        emitNullExtended(current_probe_tuple_, out);
                        unmatched_probe_emitted_++;
                        rows_output_++;
                        return true;
                    }
                    continue; // Null keys never match in INNER/RIGHT (unswapped)
                }

                encodeGroupKey(key_copies_.data(), key_copies_.size(), key_buf_);
                auto it = hash_table_.find(key_buf_);
                if (it == hash_table_.end()) {
                    if (emit_unmatched_probe_) {
                        emitNullExtended(current_probe_tuple_, out);
                        unmatched_probe_emitted_++;
                        rows_output_++;
                        return true;
                    }
                    continue; // No key match
                }
                current_build_idx_ = it->second.first; // head of chain
            }
        }

        // Walk candidate build rows for the current probe row
        while (current_build_idx_ != NO_ROW) {
            size_t b_idx = current_build_idx_;

            // Advance current_build_idx_ for the subsequent call
            if (is_nested_loop_) {
                current_build_idx_++;
                if (current_build_idx_ >= build_row_count_) {
                    current_build_idx_ = NO_ROW;
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

            // Both equi-keys and residual passed! Mark matched.
            probe_matched_ = true;
            if (emit_unmatched_build_) {
                build_matched_[b_idx] = 1;
            }

            out = combined_tuple_;
            rows_output_++;
            return true;
        }

        // Reached end of candidate build rows for this probe row
        if (!probe_matched_ && emit_unmatched_probe_) {
            emitNullExtended(current_probe_tuple_, out);
            unmatched_probe_emitted_++;
            rows_output_++;
            return true;
        }
    }
}

void HashJoinOperator::close() {
    if (join_stats_) {
        std::fprintf(stderr, "join: build rows stored %zu, probe rows read %zu, rows output %zu, unmatched probe rows %zu, unmatched build rows %zu\n",
                     build_row_count_, probe_rows_read_, rows_output_,
                     unmatched_probe_emitted_, unmatched_build_emitted_);
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
    build_matched_.clear();
    hash_table_.clear();
    build_row_count_ = 0;
    current_build_idx_ = NO_ROW;
}

const std::vector<std::string>& HashJoinOperator::columnNames() const {
    return column_names_;
}


#include "ops_agg.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>

void AggregateOperator::addFloatToSum(AggState& st, double val) {
    if (!st.sum_is_float) {
        st.sum_is_float = true;
        st.sum_d = static_cast<double>(st.sum_i);
        st.sum_c = 0.0;
    }
    // Neumaier summation: handles |val| > |sum| and preserves precision
    double t = st.sum_d + val;
    if (std::abs(st.sum_d) >= std::abs(val)) {
        st.sum_c += (st.sum_d - t) + val;
    } else {
        st.sum_c += (val - t) + st.sum_d;
    }
    st.sum_d = t;
}

void AggregateOperator::addIntToSum(AggState& st, long long val) {
    if (st.sum_is_float) {
        addFloatToSum(st, static_cast<double>(val));
    } else {
        long long res;
        if (__builtin_add_overflow(st.sum_i, val, &res)) {
            // Integer addition overflowed: mark overflow and promote to double for AVG
            st.sum_overflow = true;
            addFloatToSum(st, static_cast<double>(val));
        } else {
            st.sum_i = res;
        }
    }
}


void AggregateOperator::encodeKey(const std::vector<Value>& values, std::string& buf) {
    encodeGroupKey(values, buf);
}

AggregateOperator::AggregateOperator(
    std::unique_ptr<Operator> child,
    std::vector<std::unique_ptr<BoundExpr>> group_by_exprs,
    std::vector<AggSpec> agg_specs,
    size_t max_groups
)
    : child_(std::move(child)),
      group_by_exprs_(std::move(group_by_exprs)),
      agg_specs_(std::move(agg_specs)),
      max_groups_(max_groups) {
    // Generate internal column names for aggregate output tuple slots
    for (size_t i = 0; i < group_by_exprs_.size(); ++i) {
        output_col_names_.push_back("group_" + std::to_string(i));
    }
    for (size_t i = 0; i < agg_specs_.size(); ++i) {
        output_col_names_.push_back("agg_" + std::to_string(i));
    }
}

AggregateOperator::~AggregateOperator() {
    close();
}

bool AggregateOperator::open() {
    cursor_ = 0;
    finalized_tuples_.clear();

    if (!child_->open()) {
        return false;
    }

    size_t G = group_by_exprs_.size();
    size_t A = agg_specs_.size();

    std::unordered_map<std::string, size_t> group_map;
    std::vector<std::vector<Value>> group_keys_list;
    std::vector<AggState> flat_agg_states;

    std::string key_buf;
    std::vector<Value> current_keys(G);

    // If G == 0 (global aggregation), create group 0 immediately.
    // That ensures an empty input table still produces 1 row (e.g. COUNT(*) = 0, SUM() = NULL).
    if (G == 0) {
        group_map[""] = 0;
        group_keys_list.push_back({});
        flat_agg_states.resize(A);
    }

    Tuple child_tuple;
    while (child_->next(child_tuple)) {
        size_t group_idx = 0;
        if (G > 0) {
            for (size_t g = 0; g < G; ++g) {
                current_keys[g] = evalExpr(group_by_exprs_[g].get(), child_tuple);
            }
            encodeKey(current_keys, key_buf);
            auto it = group_map.find(key_buf);
            if (it == group_map.end()) {
                if (max_groups_ > 0 && group_map.size() >= max_groups_) {
                    std::cerr << "Error: maximum number of groups exceeded (" << max_groups_ << ")\n";
                    std::exit(4);
                }
                try {
                    group_idx = group_map.size();
                    group_map[key_buf] = group_idx;
                    group_keys_list.push_back(current_keys);
                    flat_agg_states.resize((group_idx + 1) * A);
                } catch (const std::bad_alloc&) {
                    std::cerr << "Error: out of memory during aggregation\n";
                    std::exit(4);
                }
            } else {
                group_idx = it->second;
            }
        }

        // Update aggregates for this group
        AggState* states = &flat_agg_states[group_idx * A];
        for (size_t a = 0; a < A; ++a) {
            const AggSpec& spec = agg_specs_[a];
            AggState& st = states[a];

            if (spec.func == AggFunc::COUNT_STAR) {
                st.count++;
                continue;
            }

            Value val = evalExpr(spec.arg.get(), child_tuple);
            if (val.type == V_NULL) {
                continue;
            }

            st.count++;

            switch (spec.func) {
                case AggFunc::COUNT:
                    // Count of non-null row is already incremented above
                    break;

                case AggFunc::SUM:
                case AggFunc::AVG: {
                    // Extract numeric value according to SQLite rules
                    if (val.type == V_INT) {
                        addIntToSum(st, val.i);
                    } else if (val.type == V_FLOAT) {
                        addFloatToSum(st, val.d);
                    } else if (val.type == V_STRING) {
                        Value num;
                        if (isEntirelyNumeric(val.s, num)) {
                            if (num.type == V_INT) {
                                addIntToSum(st, num.i);
                            } else {
                                addFloatToSum(st, num.d);
                            }
                        } else {
                            // Non-numeric text or trailing non-numeric characters:
                            // SQLite converts numeric prefix to REAL (0.0 if no prefix)
                            Value prefix_num = toNumber(val);
                            double d = 0.0;
                            if (prefix_num.type == V_INT) {
                                d = static_cast<double>(prefix_num.i);
                            } else if (prefix_num.type == V_FLOAT) {
                                d = prefix_num.d;
                            }
                            addFloatToSum(st, d);
                        }
                    }
                    break;
                }

                case AggFunc::MIN: {
                    if (!st.has_val) {
                        st.has_val = true;
                        st.min_max_val = val;
                    } else if (compareValues(val, st.min_max_val) < 0) {
                        st.min_max_val = val;
                    }
                    break;
                }

                case AggFunc::MAX: {
                    if (!st.has_val) {
                        st.has_val = true;
                        st.min_max_val = val;
                    } else if (compareValues(val, st.min_max_val) > 0) {
                        st.min_max_val = val;
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }

    // IMPORTANT: Do NOT close child_ here!
    // Group keys and MIN/MAX strings point into the mmap / arena and must stay valid.
    // child_->close() will be called in AggregateOperator::close().

    // Finalize all groups into memory buffer before returning from open()
    size_t num_groups = group_keys_list.size();
    finalized_tuples_.resize(num_groups);

    for (size_t g = 0; g < num_groups; ++g) {
        Tuple& t = finalized_tuples_[g];
        t.values.resize(G + A);

        for (size_t k = 0; k < G; ++k) {
            t.values[k] = group_keys_list[g][k];
        }

        for (size_t a = 0; a < A; ++a) {
            const AggSpec& spec = agg_specs_[a];
            const AggState& st = flat_agg_states[g * A + a];

            switch (spec.func) {
                case AggFunc::COUNT_STAR:
                case AggFunc::COUNT:
                    t.values[G + a] = makeInt(st.count);
                    break;

                case AggFunc::SUM: {
                    if (st.sum_overflow) {
                        std::cerr << "Error: integer overflow in SUM()\n";
                        std::exit(4);
                    }
                    if (st.count == 0) {
                        t.values[G + a] = makeNull();
                    } else if (st.sum_is_float) {
                        t.values[G + a] = makeFloat(st.sum_d + st.sum_c);
                    } else {
                        t.values[G + a] = makeInt(st.sum_i);
                    }
                    break;
                }

                case AggFunc::AVG: {
                    if (st.count == 0) {
                        t.values[G + a] = makeNull();
                    } else {
                        double s = st.sum_is_float ? (st.sum_d + st.sum_c) : static_cast<double>(st.sum_i);
                        t.values[G + a] = makeFloat(s / static_cast<double>(st.count));
                    }
                    break;
                }

                case AggFunc::MIN:
                case AggFunc::MAX: {
                    if (!st.has_val) {
                        t.values[G + a] = makeNull();
                    } else {
                        t.values[G + a] = st.min_max_val;
                    }
                    break;
                }
            }
        }
    }

    return true;
}

bool AggregateOperator::next(Tuple& out) {
    if (cursor_ >= finalized_tuples_.size()) {
        return false;
    }
    out = finalized_tuples_[cursor_++];
    return true;
}

void AggregateOperator::close() {
    finalized_tuples_.clear();
    cursor_ = 0;
    if (child_) {
        child_->close();
    }
}

const std::vector<std::string>& AggregateOperator::columnNames() const {
    return output_col_names_;
}

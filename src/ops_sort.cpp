#include "ops_sort.h"

#include <algorithm>
#include <iostream>
#include <cstdlib>

SortOperator::SortOperator(
    std::unique_ptr<Operator> child,
    std::vector<SortTerm> sort_terms,
    long long top_n
)
    : child_(std::move(child)),
      sort_terms_(std::move(sort_terms)),
      top_n_(top_n) {}

SortOperator::~SortOperator() {
    close();
}

bool SortOperator::isBetter(const HeapEntry& a, const HeapEntry& b) const {
    for (const auto& term : sort_terms_) {
        Value va = a.values[term.slot_idx];
        Value vb = b.values[term.slot_idx];
        int cmp = compareValuesWithNull(va, vb);
        if (cmp != 0) {
            return term.descending ? (cmp > 0) : (cmp < 0);
        }
    }
    // Stable tie-breaker: earlier arrival sequence number is better
    return a.seq < b.seq;
}

void SortOperator::siftUp(size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        // In our max-heap, parent must be worse than or equal to child.
        // If parent is better than child, swap them so worse element moves up.
        if (isBetter(heap_entries_[parent], heap_entries_[idx])) {
            std::swap(heap_entries_[parent], heap_entries_[idx]);
            idx = parent;
        } else {
            break;
        }
    }
}

void SortOperator::siftDown(size_t idx, size_t n) {
    while (2 * idx + 1 < n) {
        size_t worst_child = 2 * idx + 1;
        size_t right = worst_child + 1;
        // Find worst child (the one that is worse, i.e., not better)
        if (right < n && isBetter(heap_entries_[worst_child], heap_entries_[right])) {
            worst_child = right;
        }
        // If parent is better than worst_child, swap so worse element is on top
        if (isBetter(heap_entries_[idx], heap_entries_[worst_child])) {
            std::swap(heap_entries_[idx], heap_entries_[worst_child]);
            idx = worst_child;
        } else {
            break;
        }
    }
}

bool SortOperator::open() {
    cursor_ = 0;
    max_entries_held_ = 0;
    flat_values_.clear();
    permutation_.clear();
    heap_entries_.clear();

    if (!child_ || !child_->open()) {
        return false;
    }

    width_ = child_->columnNames().size();

    // Mode B: TOP-N with top_n == 0 (e.g. LIMIT 0)
    // Must open child cleanly but must not pull any rows from child.
    if (top_n_ == 0) {
        return true;
    }

    Tuple child_tuple;

    if (top_n_ > 0) {
        // ---------------------------------------------------------------------
        // Mode B: TOP-N Bounded Max-Heap (capacity = top_n_)
        // ---------------------------------------------------------------------
        size_t current_seq = 0;
        size_t cap = static_cast<size_t>(top_n_);

        while (child_->next(child_tuple)) {
            size_t seq = current_seq++;

            if (heap_entries_.size() < cap) {
                try {
                    HeapEntry entry;
                    entry.seq = seq;
                    entry.values = child_tuple.values;
                    heap_entries_.push_back(std::move(entry));
                    siftUp(heap_entries_.size() - 1);
                    if (heap_entries_.size() > max_entries_held_) {
                        max_entries_held_ = heap_entries_.size();
                    }
                } catch (const std::bad_alloc&) {
                    std::cerr << "Error: out of memory while sorting (" << heap_entries_.size()
                              << " rows buffered); use LIMIT or see external sort (not implemented yet)\n";
                    std::exit(4);
                }
            } else {
                // Heap is full: compare incoming row with root (the worst kept row).
                // Incoming row replaces root ONLY if incoming row is strictly better.
                bool incoming_better = false;
                for (const auto& term : sort_terms_) {
                    Value va = child_tuple.values[term.slot_idx];
                    Value vb = heap_entries_[0].values[term.slot_idx];
                    int cmp = compareValuesWithNull(va, vb);
                    if (cmp != 0) {
                        incoming_better = term.descending ? (cmp > 0) : (cmp < 0);
                        break;
                    }
                }
                // Later arriving row that ties on all keys has seq > root.seq, so it is worse (not inserted).
                if (incoming_better) {
                    heap_entries_[0].seq = seq;
                    heap_entries_[0].values = child_tuple.values; // vector copy reuses storage
                    siftDown(0, heap_entries_.size());
                }
            }
        }

        // Sort kept entries best-first for output streaming
        std::sort(heap_entries_.begin(), heap_entries_.end(), [this](const HeapEntry& a, const HeapEntry& b) {
            return isBetter(a, b);
        });

        return true;
    }

    // -------------------------------------------------------------------------
    // Mode A: FULL SORT
    // -------------------------------------------------------------------------
    // Memory cost per row: width * sizeof(Value) + sizeof(size_t) (permutation entry)
    try {
        while (child_->next(child_tuple)) {
            size_t row_idx = permutation_.size();
            permutation_.push_back(row_idx);
            for (size_t c = 0; c < width_; ++c) {
                flat_values_.push_back(child_tuple.values[c]);
            }
        }
    } catch (const std::bad_alloc&) {
        std::cerr << "Error: out of memory while sorting (" << permutation_.size()
                  << " rows buffered); use LIMIT or see external sort (not implemented yet)\n";
        std::exit(4);
    }

    max_entries_held_ = permutation_.size();

    // Comparator sorting permutation indices
    struct PermutationComparator {
        const std::vector<Value>& flat;
        const std::vector<SortTerm>& terms;
        size_t width;

        bool operator()(size_t row_a, size_t row_b) const {
            for (const auto& term : terms) {
                Value va = flat[row_a * width + term.slot_idx];
                Value vb = flat[row_b * width + term.slot_idx];
                int cmp = compareValuesWithNull(va, vb);
                if (cmp != 0) {
                    return term.descending ? (cmp > 0) : (cmp < 0);
                }
            }
            // Deterministic stable tie-breaker: arrival sequence number
            return row_a < row_b;
        }
    };

    PermutationComparator comp{flat_values_, sort_terms_, width_};
    std::sort(permutation_.begin(), permutation_.end(), comp);

    return true;
}

bool SortOperator::next(Tuple& out) {
    if (top_n_ > 0) {
        if (cursor_ >= heap_entries_.size()) {
            return false;
        }
        out.values = heap_entries_[cursor_++].values;
        return true;
    }

    if (top_n_ == 0 || cursor_ >= permutation_.size()) {
        return false;
    }

    size_t row_idx = permutation_[cursor_++];
    if (out.values.size() != width_) {
        out.values.resize(width_, makeNull());
    }
    for (size_t c = 0; c < width_; ++c) {
        out.values[c] = flat_values_[row_idx * width_ + c];
    }
    return true;
}

void SortOperator::close() {
    cursor_ = 0;
    flat_values_.clear();
    permutation_.clear();
    heap_entries_.clear();
    // Do NOT close child in open(); keep open until own close() so string_views don't dangle
    if (child_) {
        child_->close();
    }
}

const std::vector<std::string>& SortOperator::columnNames() const {
    return child_->columnNames();
}


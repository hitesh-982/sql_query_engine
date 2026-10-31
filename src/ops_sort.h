#ifndef OPS_SORT_H
#define OPS_SORT_H

#include "operator.h"
#include "value.h"

#include <vector>
#include <memory>
#include <cstddef>
#include <string>

struct SortTerm {
    size_t slot_idx = 0;
    bool descending = false;
};

class SortOperator : public Operator {
public:
    SortOperator(
        std::unique_ptr<Operator> child,
        std::vector<SortTerm> sort_terms,
        long long top_n = -1
    );
    ~SortOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;

    const std::vector<std::string>& columnNames() const override;

    // Debug counter: maximum entries ever held in memory
    size_t maxEntriesEverHeld() const { return max_entries_held_; }

private:
    struct HeapEntry {
        size_t seq = 0;
        std::vector<Value> values;
    };

    bool isBetter(const HeapEntry& a, const HeapEntry& b) const;
    void siftUp(size_t idx);
    void siftDown(size_t idx, size_t n);

    std::unique_ptr<Operator> child_;
    std::vector<SortTerm> sort_terms_;
    long long top_n_; // -1 for Full Sort; >= 0 for Top-N
    size_t width_ = 0;
    size_t cursor_ = 0;
    size_t max_entries_held_ = 0;

    // Storage for Mode A (Full Sort)
    std::vector<Value> flat_values_;
    std::vector<size_t> permutation_;

    // Storage for Mode B (Top-N Heap)
    std::vector<HeapEntry> heap_entries_;
};

#endif // OPS_SORT_H


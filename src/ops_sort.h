#ifndef OPS_SORT_H
#define OPS_SORT_H

#include "operator.h"
#include "value.h"

#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>

struct SortTerm {
    size_t slot_idx = 0;
    bool descending = false;
};

// ----------------------------------------------------------------------------
// RunWriter: writes binary sorted records to an open file descriptor
// Formats each record:
//   uint32_t payload_len;
//   uint64_t seq;
//   for each slot:
//     uint8_t type;
//     payload (INT: 8 bytes, FLOAT: 8 bytes, STRING: 4-byte len + bytes, NULL: 0)
// Writes through a 1 MB user-space buffer.
// ----------------------------------------------------------------------------
class RunWriter {
public:
    RunWriter(int fd, size_t buf_capacity = 1024 * 1024);
    ~RunWriter();

    void writeRecord(size_t seq, const Value* values, size_t width);
    void flush();
    size_t bytesWritten() const { return bytes_written_; }

private:
    void writeExact(const void* data, size_t len);

    int fd_;
    std::vector<char> buf_;
    size_t buf_pos_ = 0;
    size_t bytes_written_ = 0;
    std::vector<char> record_scratch_;
};

// ----------------------------------------------------------------------------
// RunReader: buffered binary reader for sorted run files.
// Reads whole record into record_buf_, then builds string_views into it.
// Owns current record storage; values are valid until nextRecord() is called.
// ----------------------------------------------------------------------------
class RunReader {
public:
    RunReader(int fd, size_t width, size_t buf_capacity = 64 * 1024);
    ~RunReader();

    RunReader(const RunReader&) = delete;
    RunReader& operator=(const RunReader&) = delete;
    RunReader(RunReader&& other) noexcept;
    RunReader& operator=(RunReader&& other) noexcept;

    bool nextRecord();
    size_t currentSeq() const { return current_seq_; }
    const Value* currentValues() const { return current_values_.data(); }
    const std::vector<Value>& currentTuple() const { return current_values_; }
    void close();

private:
    bool readExact(void* dst, size_t len);

    int fd_ = -1;
    size_t width_ = 0;
    std::vector<char> io_buf_;
    size_t io_pos_ = 0;
    size_t io_avail_ = 0;

    size_t current_seq_ = 0;
    std::vector<Value> current_values_;
    std::vector<char> record_buf_;
};

// ----------------------------------------------------------------------------
// SortOperator: Volcano operator for ORDER BY.
// Supports:
//   - Mode A (Full Sort): In-memory permutation sort when fitting in RAM,
//     spilling to an external merge sort with level-based cascading merge when
//     buffered rows exceed memory_limit_bytes.
//   - Mode B (Top-N): Bounded max-heap keeping at most top_n rows in RAM.
//     Top-N never spills to disk.
// ----------------------------------------------------------------------------
class SortOperator : public Operator {
public:
    SortOperator(
        std::unique_ptr<Operator> child,
        std::vector<SortTerm> sort_terms,
        long long top_n = -1,
        size_t memory_limit_bytes = 512ULL * 1024 * 1024,
        std::string tmp_dir = "/tmp",
        bool sort_stats = false,
        size_t fan_in = 64
    );
    ~SortOperator() override;

    bool open() override;
    bool next(Tuple& out) override;
    void close() override;

    const std::vector<std::string>& columnNames() const override;

    // Debug counters
    size_t maxEntriesEverHeld() const { return max_entries_held_; }
    size_t initialRunsCount() const { return total_initial_runs_; }
    size_t mergePassesCount() const { return total_merge_passes_; }
    size_t spilledBytesCount() const { return total_spilled_bytes_; }

private:
    struct HeapEntry {
        size_t seq = 0;
        std::vector<Value> values;
    };

    // Mode B: Top-N Heap methods
    bool isBetter(const HeapEntry& a, const HeapEntry& b) const;
    void siftUp(size_t idx);
    void siftDown(size_t idx, size_t n);

    // Mode A: External merge sort helpers
    int createTempFile();
    int mergeRunsToFile(const std::vector<int>& input_fds);
    void addRunToLevel(int fd, size_t level);
    void spillCurrentBatch();

    // Min-heap for streaming final merge in next()
    void pushMergeHeap(size_t run_idx);
    size_t popMergeHeap();
    void siftUpMergeHeap(size_t idx);
    void siftDownMergeHeap(size_t idx);
    bool isReaderBetter(size_t run_a, size_t run_b) const;

    std::unique_ptr<Operator> child_;
    std::vector<SortTerm> sort_terms_;
    long long top_n_; // -1 for Full Sort; >= 0 for Top-N
    size_t width_ = 0;
    size_t cursor_ = 0;
    size_t max_entries_held_ = 0;

    // External sort configuration
    size_t memory_limit_bytes_;
    std::string tmp_dir_;
    bool sort_stats_;
    size_t fan_in_;

    // Mode A (In-memory buffer & batch spill)
    std::vector<Value> flat_values_;
    std::vector<size_t> permutation_;
    std::vector<size_t> row_seqs_;

    // Leveled runs: levels_[L] holds open FDs for pending runs at level L
    std::vector<std::vector<int>> levels_;

    // Final merge streaming state
    bool external_mode_ = false;
    bool has_run_ = false;
    int last_returned_run_idx_ = -1;
    std::vector<RunReader> readers_;
    std::vector<size_t> merge_heap_;

    // Statistics
    size_t total_initial_runs_ = 0;
    size_t total_merge_passes_ = 0;
    size_t total_spilled_bytes_ = 0;

    // Mode B (Top-N Heap storage)
    std::vector<HeapEntry> heap_entries_;
};

#endif // OPS_SORT_H

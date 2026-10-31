#include "ops_sort.h"

#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// Validate that sizeof(Value) is 40 bytes on 64-bit systems
static_assert(sizeof(Value) == 40, "sizeof(Value) must be 40 bytes");

// ----------------------------------------------------------------------------
// Unified Row Comparator: Used by in-memory sort, intermediate merge, and final merge.
// Returns:
//   < 0 if row A is better than row B (A should come before B)
//   > 0 if row B is better than row A (B should come before A)
//   0 if row A and row B tie on all terms and have identical sequence numbers
// ----------------------------------------------------------------------------
static inline int compareSortRows(
    const Value* row_a, size_t seq_a,
    const Value* row_b, size_t seq_b,
    const SortTerm* terms, size_t num_terms
) {
    for (size_t t = 0; t < num_terms; ++t) {
        const auto& term = terms[t];
        Value va = row_a[term.slot_idx];
        Value vb = row_b[term.slot_idx];
        int cmp = compareValuesWithNull(va, vb);
        if (cmp != 0) {
            return term.descending ? -cmp : cmp;
        }
    }
    // Stable tie-breaker: earlier arrival sequence number is better
    if (seq_a < seq_b) return -1;
    if (seq_a > seq_b) return 1;
    return 0;
}

// ----------------------------------------------------------------------------
// RunWriter implementation
// ----------------------------------------------------------------------------
RunWriter::RunWriter(int fd, size_t buf_capacity)
    : fd_(fd),
      buf_(buf_capacity, 0),
      buf_pos_(0),
      bytes_written_(0) {
    record_scratch_.reserve(4096);
}

RunWriter::~RunWriter() {
    flush();
}

void RunWriter::writeExact(const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    while (len > 0) {
        size_t avail = buf_.size() - buf_pos_;
        if (avail == 0) {
            flush();
            avail = buf_.size();
        }
        size_t to_copy = std::min(len, avail);
        std::memcpy(buf_.data() + buf_pos_, ptr, to_copy);
        buf_pos_ += to_copy;
        ptr += to_copy;
        len -= to_copy;
    }
}

void RunWriter::writeRecord(size_t seq, const Value* values, size_t width) {
    record_scratch_.clear();

    // 1. Sequence number (8 bytes)
    uint64_t seq_val = static_cast<uint64_t>(seq);
    const char* seq_bytes = reinterpret_cast<const char*>(&seq_val);
    record_scratch_.insert(record_scratch_.end(), seq_bytes, seq_bytes + sizeof(seq_val));

    // 2. Columns
    for (size_t c = 0; c < width; ++c) {
        const Value& val = values[c];
        uint8_t type = static_cast<uint8_t>(val.type);
        record_scratch_.push_back(static_cast<char>(type));

        switch (val.type) {
            case V_NULL:
                // No payload for NULL
                break;
            case V_INT: {
                int64_t i_val = val.i;
                const char* p = reinterpret_cast<const char*>(&i_val);
                record_scratch_.insert(record_scratch_.end(), p, p + sizeof(i_val));
                break;
            }
            case V_FLOAT: {
                double d_val = val.d;
                const char* p = reinterpret_cast<const char*>(&d_val);
                record_scratch_.insert(record_scratch_.end(), p, p + sizeof(d_val));
                break;
            }
            case V_STRING: {
                uint32_t str_len = static_cast<uint32_t>(val.s.size());
                const char* p = reinterpret_cast<const char*>(&str_len);
                record_scratch_.insert(record_scratch_.end(), p, p + sizeof(str_len));
                if (str_len > 0) {
                    record_scratch_.insert(record_scratch_.end(), val.s.data(), val.s.data() + str_len);
                }
                break;
            }
        }
    }

    // Write record length header (4 bytes), then payload
    uint32_t payload_len = static_cast<uint32_t>(record_scratch_.size());
    writeExact(&payload_len, sizeof(payload_len));
    writeExact(record_scratch_.data(), payload_len);
}

void RunWriter::flush() {
    if (buf_pos_ == 0) return;
    const char* ptr = buf_.data();
    size_t remaining = buf_pos_;
    while (remaining > 0) {
        ssize_t n = ::write(fd_, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Error: failed to write to temporary file: " << std::strerror(errno) << "\n";
            std::exit(4);
        }
        if (n == 0) {
            std::cerr << "Error: zero bytes written to temporary file (disk full?)\n";
            std::exit(4);
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    bytes_written_ += buf_pos_;
    buf_pos_ = 0;
}

// ----------------------------------------------------------------------------
// RunReader implementation
// ----------------------------------------------------------------------------
RunReader::RunReader(int fd, size_t width, size_t buf_capacity)
    : fd_(fd),
      width_(width),
      io_buf_(buf_capacity, 0),
      io_pos_(0),
      io_avail_(0),
      current_seq_(0),
      current_values_(width, makeNull()) {
    record_buf_.reserve(4096);
}

RunReader::~RunReader() {
    close();
}

RunReader::RunReader(RunReader&& other) noexcept
    : fd_(other.fd_),
      width_(other.width_),
      io_buf_(std::move(other.io_buf_)),
      io_pos_(other.io_pos_),
      io_avail_(other.io_avail_),
      current_seq_(other.current_seq_),
      current_values_(std::move(other.current_values_)),
      record_buf_(std::move(other.record_buf_)) {
    other.fd_ = -1;
    // Re-point string_views in current_values_ to moved record_buf_
    if (!record_buf_.empty()) {
        const char* ptr = record_buf_.data() + sizeof(uint64_t);
        for (size_t c = 0; c < width_; ++c) {
            uint8_t type = static_cast<uint8_t>(*ptr++);
            switch (type) {
                case V_NULL: break;
                case V_INT: ptr += sizeof(int64_t); break;
                case V_FLOAT: ptr += sizeof(double); break;
                case V_STRING: {
                    uint32_t str_len;
                    std::memcpy(&str_len, ptr, sizeof(str_len));
                    ptr += sizeof(str_len);
                    current_values_[c].s = std::string_view(ptr, str_len);
                    ptr += str_len;
                    break;
                }
            }
        }
    }
}

RunReader& RunReader::operator=(RunReader&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        width_ = other.width_;
        io_buf_ = std::move(other.io_buf_);
        io_pos_ = other.io_pos_;
        io_avail_ = other.io_avail_;
        current_seq_ = other.current_seq_;
        current_values_ = std::move(other.current_values_);
        record_buf_ = std::move(other.record_buf_);
        other.fd_ = -1;

        if (!record_buf_.empty()) {
            const char* ptr = record_buf_.data() + sizeof(uint64_t);
            for (size_t c = 0; c < width_; ++c) {
                uint8_t type = static_cast<uint8_t>(*ptr++);
                switch (type) {
                    case V_NULL: break;
                    case V_INT: ptr += sizeof(int64_t); break;
                    case V_FLOAT: ptr += sizeof(double); break;
                    case V_STRING: {
                        uint32_t str_len;
                        std::memcpy(&str_len, ptr, sizeof(str_len));
                        ptr += sizeof(str_len);
                        current_values_[c].s = std::string_view(ptr, str_len);
                        ptr += str_len;
                        break;
                    }
                }
            }
        }
    }
    return *this;
}

bool RunReader::readExact(void* dst, size_t len) {
    char* out = static_cast<char*>(dst);
    while (len > 0) {
        if (io_pos_ >= io_avail_) {
            ssize_t n = 0;
            while (true) {
                n = ::read(fd_, io_buf_.data(), io_buf_.size());
                if (n < 0) {
                    if (errno == EINTR) continue;
                    std::cerr << "Error: failed to read from temporary file: " << std::strerror(errno) << "\n";
                    std::exit(4);
                }
                break;
            }
            if (n == 0) {
                return false; // Clean EOF
            }
            io_pos_ = 0;
            io_avail_ = static_cast<size_t>(n);
        }
        size_t available = io_avail_ - io_pos_;
        size_t to_copy = std::min(len, available);
        std::memcpy(out, io_buf_.data() + io_pos_, to_copy);
        io_pos_ += to_copy;
        out += to_copy;
        len -= to_copy;
    }
    return true;
}

bool RunReader::nextRecord() {
    uint32_t payload_len = 0;
    if (!readExact(&payload_len, sizeof(payload_len))) {
        return false; // Clean EOF at record boundary
    }

    if (record_buf_.size() < payload_len) {
        record_buf_.resize(payload_len);
    }

    if (!readExact(record_buf_.data(), payload_len)) {
        std::cerr << "Error: unexpected EOF reading record payload from temporary file\n";
        std::exit(4);
    }

    const char* ptr = record_buf_.data();
    const char* end = ptr + payload_len;

    // Sequence number (8 bytes)
    if (ptr + sizeof(uint64_t) > end) {
        std::cerr << "Error: corrupt record in temporary file (truncated sequence number)\n";
        std::exit(4);
    }
    uint64_t seq_val;
    std::memcpy(&seq_val, ptr, sizeof(seq_val));
    current_seq_ = static_cast<size_t>(seq_val);
    ptr += sizeof(seq_val);

    // Columns
    for (size_t c = 0; c < width_; ++c) {
        if (ptr >= end) {
            std::cerr << "Error: corrupt record in temporary file (missing column type)\n";
            std::exit(4);
        }
        uint8_t type = static_cast<uint8_t>(*ptr++);
        switch (type) {
            case V_NULL:
                current_values_[c] = makeNull();
                break;
            case V_INT: {
                if (ptr + sizeof(int64_t) > end) {
                    std::cerr << "Error: corrupt record in temporary file (truncated INT)\n";
                    std::exit(4);
                }
                int64_t i_val;
                std::memcpy(&i_val, ptr, sizeof(i_val));
                ptr += sizeof(i_val);
                current_values_[c] = makeInt(i_val);
                break;
            }
            case V_FLOAT: {
                if (ptr + sizeof(double) > end) {
                    std::cerr << "Error: corrupt record in temporary file (truncated FLOAT)\n";
                    std::exit(4);
                }
                double d_val;
                std::memcpy(&d_val, ptr, sizeof(d_val));
                ptr += sizeof(d_val);
                current_values_[c] = makeFloat(d_val);
                break;
            }
            case V_STRING: {
                if (ptr + sizeof(uint32_t) > end) {
                    std::cerr << "Error: corrupt record in temporary file (truncated STRING length)\n";
                    std::exit(4);
                }
                uint32_t str_len;
                std::memcpy(&str_len, ptr, sizeof(str_len));
                ptr += sizeof(str_len);
                if (ptr + str_len > end) {
                    std::cerr << "Error: corrupt record in temporary file (truncated STRING bytes)\n";
                    std::exit(4);
                }
                current_values_[c] = makeString(std::string_view(ptr, str_len));
                ptr += str_len;
                break;
            }
            default:
                std::cerr << "Error: corrupt record in temporary file (unknown ValueType "
                          << static_cast<int>(type) << ")\n";
                std::exit(4);
        }
    }
    return true;
}

void RunReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ----------------------------------------------------------------------------
// SortOperator implementation
// ----------------------------------------------------------------------------
SortOperator::SortOperator(
    std::unique_ptr<Operator> child,
    std::vector<SortTerm> sort_terms,
    long long top_n,
    size_t memory_limit_bytes,
    std::string tmp_dir,
    bool sort_stats,
    size_t fan_in
)
    : child_(std::move(child)),
      sort_terms_(std::move(sort_terms)),
      top_n_(top_n),
      memory_limit_bytes_(memory_limit_bytes),
      tmp_dir_(std::move(tmp_dir)),
      sort_stats_(sort_stats),
      fan_in_(fan_in) {}

SortOperator::~SortOperator() {
    close();
}

int SortOperator::createTempFile() {
    char path_template[PATH_MAX];
    std::snprintf(path_template, sizeof(path_template), "%s/sql_sort_XXXXXX", tmp_dir_.c_str());
    int fd = ::mkstemp(path_template);
    if (fd < 0) {
        std::cerr << "Error: failed to create temporary file in '" << tmp_dir_ << "': "
                  << std::strerror(errno) << "\n";
        std::exit(4);
    }
    // POSIX immediate unlink: file is deleted when closed or on process exit
    if (::unlink(path_template) < 0) {
        std::cerr << "Error: failed to unlink temporary file '" << path_template << "': "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        std::exit(4);
    }
    return fd;
}

bool SortOperator::isBetter(const HeapEntry& a, const HeapEntry& b) const {
    return compareSortRows(
        a.values.data(), a.seq,
        b.values.data(), b.seq,
        sort_terms_.data(), sort_terms_.size()
    ) < 0;
}

void SortOperator::siftUp(size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
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
        if (right < n && isBetter(heap_entries_[worst_child], heap_entries_[right])) {
            worst_child = right;
        }
        if (isBetter(heap_entries_[idx], heap_entries_[worst_child])) {
            std::swap(heap_entries_[idx], heap_entries_[worst_child]);
            idx = worst_child;
        } else {
            break;
        }
    }
}

bool SortOperator::isReaderBetter(size_t run_a, size_t run_b) const {
    return compareSortRows(
        readers_[run_a].currentValues(), readers_[run_a].currentSeq(),
        readers_[run_b].currentValues(), readers_[run_b].currentSeq(),
        sort_terms_.data(), sort_terms_.size()
    ) < 0;
}

void SortOperator::siftUpMergeHeap(size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (isReaderBetter(merge_heap_[idx], merge_heap_[parent])) {
            std::swap(merge_heap_[parent], merge_heap_[idx]);
            idx = parent;
        } else {
            break;
        }
    }
}

void SortOperator::siftDownMergeHeap(size_t idx) {
    size_t n = merge_heap_.size();
    while (2 * idx + 1 < n) {
        size_t best_child = 2 * idx + 1;
        size_t right = best_child + 1;
        if (right < n && isReaderBetter(merge_heap_[right], merge_heap_[best_child])) {
            best_child = right;
        }
        if (isReaderBetter(merge_heap_[best_child], merge_heap_[idx])) {
            std::swap(merge_heap_[idx], merge_heap_[best_child]);
            idx = best_child;
        } else {
            break;
        }
    }
}

void SortOperator::pushMergeHeap(size_t run_idx) {
    merge_heap_.push_back(run_idx);
    siftUpMergeHeap(merge_heap_.size() - 1);
}

size_t SortOperator::popMergeHeap() {
    size_t best = merge_heap_[0];
    merge_heap_[0] = merge_heap_.back();
    merge_heap_.pop_back();
    if (!merge_heap_.empty()) {
        siftDownMergeHeap(0);
    }
    return best;
}

// ----------------------------------------------------------------------------
// Intermediate K-way merge: merges input_fds into a single new run file
// ----------------------------------------------------------------------------
int SortOperator::mergeRunsToFile(const std::vector<int>& input_fds) {
    int out_fd = createTempFile();
    RunWriter writer(out_fd);

    std::vector<RunReader> readers;
    readers.reserve(input_fds.size());
    for (int fd : input_fds) {
        readers.emplace_back(fd, width_);
    }

    struct InterHeapComp {
        const std::vector<RunReader>& r;
        const SortTerm* terms;
        size_t num_terms;

        bool isBetter(size_t a, size_t b) const {
            return compareSortRows(
                r[a].currentValues(), r[a].currentSeq(),
                r[b].currentValues(), r[b].currentSeq(),
                terms, num_terms
            ) < 0;
        }
    };

    InterHeapComp comp{readers, sort_terms_.data(), sort_terms_.size()};
    std::vector<size_t> heap;
    heap.reserve(readers.size());

    for (size_t i = 0; i < readers.size(); ++i) {
        if (readers[i].nextRecord()) {
            heap.push_back(i);
            // Sift up
            size_t idx = heap.size() - 1;
            while (idx > 0) {
                size_t parent = (idx - 1) / 2;
                if (comp.isBetter(heap[idx], heap[parent])) {
                    std::swap(heap[parent], heap[idx]);
                    idx = parent;
                } else {
                    break;
                }
            }
        }
    }

    while (!heap.empty()) {
        size_t best_run = heap[0];
        // CRITICAL: Write record before advancing reader so string views remain valid
        writer.writeRecord(readers[best_run].currentSeq(), readers[best_run].currentValues(), width_);

        if (readers[best_run].nextRecord()) {
            // Sift down
            size_t idx = 0;
            size_t n = heap.size();
            while (2 * idx + 1 < n) {
                size_t best_child = 2 * idx + 1;
                size_t right = best_child + 1;
                if (right < n && comp.isBetter(heap[right], heap[best_child])) {
                    best_child = right;
                }
                if (comp.isBetter(heap[best_child], heap[idx])) {
                    std::swap(heap[idx], heap[best_child]);
                    idx = best_child;
                } else {
                    break;
                }
            }
        } else {
            // Run exhausted: pop from heap
            heap[0] = heap.back();
            heap.pop_back();
            if (!heap.empty()) {
                size_t idx = 0;
                size_t n = heap.size();
                while (2 * idx + 1 < n) {
                    size_t best_child = 2 * idx + 1;
                    size_t right = best_child + 1;
                    if (right < n && comp.isBetter(heap[right], heap[best_child])) {
                        best_child = right;
                    }
                    if (comp.isBetter(heap[best_child], heap[idx])) {
                        std::swap(heap[idx], heap[best_child]);
                        idx = best_child;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    writer.flush();
    total_spilled_bytes_ += writer.bytesWritten();
    total_merge_passes_++;

    // Close and release all input FDs
    for (auto& r : readers) {
        r.close();
    }

    if (::lseek(out_fd, 0, SEEK_SET) < 0) {
        std::cerr << "Error: failed to seek temporary file: " << std::strerror(errno) << "\n";
        std::exit(4);
    }
    return out_fd;
}

// ----------------------------------------------------------------------------
// Leveled merge: Adds run to level; when fan_in_ runs accumulate, merges up
// ----------------------------------------------------------------------------
void SortOperator::addRunToLevel(int fd, size_t level) {
    if (level >= levels_.size()) {
        levels_.resize(level + 1);
    }
    levels_[level].push_back(fd);
    if (levels_[level].size() >= fan_in_) {
        int merged_fd = mergeRunsToFile(levels_[level]);
        levels_[level].clear();
        addRunToLevel(merged_fd, level + 1);
    }
}

// ----------------------------------------------------------------------------
// Spill in-memory batch to a new level 0 run
// ----------------------------------------------------------------------------
void SortOperator::spillCurrentBatch() {
    if (permutation_.empty()) return;

    struct BatchComparator {
        const Value* flat;
        const size_t* seqs;
        size_t width;
        const SortTerm* terms;
        size_t num_terms;

        bool operator()(size_t a, size_t b) const {
            return compareSortRows(
                flat + a * width, seqs[a],
                flat + b * width, seqs[b],
                terms, num_terms
            ) < 0;
        }
    };

    BatchComparator comp{
        flat_values_.data(),
        row_seqs_.data(),
        width_,
        sort_terms_.data(),
        sort_terms_.size()
    };
    std::sort(permutation_.begin(), permutation_.end(), comp);

    int out_fd = createTempFile();
    RunWriter writer(out_fd);

    for (size_t row_idx : permutation_) {
        writer.writeRecord(row_seqs_[row_idx], &flat_values_[row_idx * width_], width_);
    }
    writer.flush();
    total_spilled_bytes_ += writer.bytesWritten();
    total_initial_runs_++;

    if (::lseek(out_fd, 0, SEEK_SET) < 0) {
        std::cerr << "Error: failed to seek temporary file: " << std::strerror(errno) << "\n";
        std::exit(4);
    }

    addRunToLevel(out_fd, 0);
}

// ----------------------------------------------------------------------------
// Operator Lifecycle
// ----------------------------------------------------------------------------
bool SortOperator::open() {
    cursor_ = 0;
    max_entries_held_ = 0;
    external_mode_ = false;
    has_run_ = true;
    last_returned_run_idx_ = -1;
    total_initial_runs_ = 0;
    total_merge_passes_ = 0;
    total_spilled_bytes_ = 0;

    flat_values_.clear();
    permutation_.clear();
    row_seqs_.clear();
    heap_entries_.clear();
    readers_.clear();
    merge_heap_.clear();
    for (auto& level : levels_) {
        for (int fd : level) {
            if (fd >= 0) ::close(fd);
        }
    }
    levels_.clear();

    if (!child_ || !child_->open()) {
        return false;
    }

    width_ = child_->columnNames().size();

    // Mode B: TOP-N with top_n == 0
    if (top_n_ == 0) {
        return true;
    }

    Tuple child_tuple;

    if (top_n_ > 0) {
        // ---------------------------------------------------------------------
        // Mode B: TOP-N Bounded Max-Heap (capacity = top_n_). Top-N never spills.
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
                    std::cerr << "Error: out of memory in Top-N sort\n";
                    std::exit(4);
                }
            } else {
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
                if (incoming_better) {
                    heap_entries_[0].seq = seq;
                    heap_entries_[0].values = child_tuple.values;
                    siftDown(0, heap_entries_.size());
                }
            }
        }

        std::sort(heap_entries_.begin(), heap_entries_.end(), [this](const HeapEntry& a, const HeapEntry& b) {
            return isBetter(a, b);
        });

        return true;
    }

    // -------------------------------------------------------------------------
    // Mode A: FULL SORT with External Merge Spilling
    // Memory per row: width * sizeof(Value) + 2 * sizeof(size_t)
    // -------------------------------------------------------------------------
    size_t row_size = width_ * sizeof(Value) + 2 * sizeof(size_t);
    size_t batch_capacity = memory_limit_bytes_ / row_size;
    if (batch_capacity == 0) {
        batch_capacity = 1;
    }

    flat_values_.reserve(batch_capacity * width_);
    permutation_.reserve(batch_capacity);
    row_seqs_.reserve(batch_capacity);

    size_t current_seq = 0;
    while (child_->next(child_tuple)) {
        if (permutation_.size() >= batch_capacity) {
            spillCurrentBatch();
            flat_values_.clear();
            permutation_.clear();
            row_seqs_.clear();
            flat_values_.reserve(batch_capacity * width_);
            permutation_.reserve(batch_capacity);
            row_seqs_.reserve(batch_capacity);
        }

        size_t row_idx = permutation_.size();
        permutation_.push_back(row_idx);
        row_seqs_.push_back(current_seq++);
        for (size_t c = 0; c < width_; ++c) {
            flat_values_.push_back(child_tuple.values[c]);
        }
    }

    if (total_initial_runs_ == 0) {
        // Case 1: Entire dataset fit in memory! No spills occurred.
        max_entries_held_ = permutation_.size();

        struct InMemComp {
            const Value* flat;
            const size_t* seqs;
            size_t width;
            const SortTerm* terms;
            size_t num_terms;

            bool operator()(size_t a, size_t b) const {
                return compareSortRows(
                    flat + a * width, seqs[a],
                    flat + b * width, seqs[b],
                    terms, num_terms
                ) < 0;
            }
        };

        InMemComp comp{
            flat_values_.data(),
            row_seqs_.data(),
            width_,
            sort_terms_.data(),
            sort_terms_.size()
        };
        std::sort(permutation_.begin(), permutation_.end(), comp);
        external_mode_ = false;
        return true;
    }

    // Case 2: One or more runs were spilled to disk.
    if (!permutation_.empty()) {
        spillCurrentBatch();
        flat_values_.clear();
        permutation_.clear();
        row_seqs_.clear();
    }

    // Collect all pending run FDs across all levels
    std::vector<int> pending_fds;
    for (auto& level : levels_) {
        for (int fd : level) {
            pending_fds.push_back(fd);
        }
        level.clear();
    }
    levels_.clear();

    // Reduce pending runs until at most fan_in_ remain
    while (pending_fds.size() > fan_in_) {
        std::vector<int> next_round;
        for (size_t i = 0; i < pending_fds.size(); i += fan_in_) {
            size_t count = std::min(fan_in_, pending_fds.size() - i);
            std::vector<int> batch(pending_fds.begin() + i, pending_fds.begin() + i + count);
            if (batch.size() == 1) {
                next_round.push_back(batch[0]);
            } else {
                next_round.push_back(mergeRunsToFile(batch));
            }
        }
        pending_fds = std::move(next_round);
    }

    // Initialize final merge readers
    readers_.reserve(pending_fds.size());
    merge_heap_.clear();

    if (pending_fds.size() > 1) {
        total_merge_passes_++; // Final streaming pass counts as a merge pass
    }

    for (int fd : pending_fds) {
        readers_.emplace_back(fd, width_);
        size_t idx = readers_.size() - 1;
        if (readers_[idx].nextRecord()) {
            pushMergeHeap(idx);
        }
    }

    external_mode_ = true;
    last_returned_run_idx_ = -1;
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

    if (top_n_ == 0) {
        return false;
    }

    if (!external_mode_) {
        // In-memory stream
        if (cursor_ >= permutation_.size()) {
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

    // External sort streaming
    // LAZY ADVANCE: Advance the reader that supplied the previous row NOW,
    // so its string_views were not overwritten while the consumer was using them.
    if (last_returned_run_idx_ >= 0) {
        size_t prev_run = static_cast<size_t>(last_returned_run_idx_);
        last_returned_run_idx_ = -1;
        if (readers_[prev_run].nextRecord()) {
            pushMergeHeap(prev_run);
        }
    }

    if (merge_heap_.empty()) {
        return false;
    }

    size_t best_run = popMergeHeap();
    out.values = readers_[best_run].currentTuple();
    last_returned_run_idx_ = static_cast<int>(best_run);
    return true;
}

void SortOperator::close() {
    cursor_ = 0;
    flat_values_.clear();
    permutation_.clear();
    row_seqs_.clear();
    heap_entries_.clear();
    merge_heap_.clear();

    for (auto& reader : readers_) {
        reader.close();
    }
    readers_.clear();

    for (auto& level : levels_) {
        for (int fd : level) {
            if (fd >= 0) ::close(fd);
        }
        level.clear();
    }
    levels_.clear();

    if (sort_stats_ && has_run_) {
        std::fprintf(stderr, "sort: %zu runs, %zu merge passes, spilled %zu bytes\n",
                     total_initial_runs_, total_merge_passes_, total_spilled_bytes_);
    }
    has_run_ = false;

    if (child_) {
        child_->close();
    }
}

const std::vector<std::string>& SortOperator::columnNames() const {
    return child_->columnNames();
}

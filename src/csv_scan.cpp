#include "csv_scan.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <charconv>
#include <iostream>
#include <algorithm>

bool isNull(std::string_view f) {
    // Empty field indicates SQL NULL per project rules
    return f.empty();
}

bool parseInt(std::string_view f, long long& out) {
    while (!f.empty() && (f.front() == ' ' || f.front() == '\t')) f.remove_prefix(1);
    while (!f.empty() && (f.back() == ' ' || f.back() == '\t')) f.remove_suffix(1);
    if (f.empty()) return false;

    // std::from_chars is fast and avoids heap allocation / locale overhead
    auto [ptr, ec] = std::from_chars(f.data(), f.data() + f.size(), out);
    return ec == std::errc{} && ptr == f.data() + f.size();
}

bool parseDouble(std::string_view f, double& out) {
    while (!f.empty() && (f.front() == ' ' || f.front() == '\t')) f.remove_prefix(1);
    while (!f.empty() && (f.back() == ' ' || f.back() == '\t')) f.remove_suffix(1);
    if (f.empty()) return false;

    // Reject NaN, Inf, Infinity (SQLite treats them as TEXT)
    std::string_view check = f;
    if (check.front() == '+' || check.front() == '-') {
        check.remove_prefix(1);
    }
    if (check.empty() || (!(check.front() >= '0' && check.front() <= '9') && check.front() != '.')) {
        return false;
    }
    if (check.size() >= 2 && check[0] == '0' && (check[1] == 'x' || check[1] == 'X')) {
        return false;
    }

    // std::from_chars for double is supported by modern POSIX C++17 compilers (Clang/GCC).
    // It avoids locale lookups and does not require a null-terminated string buffer.
    auto [ptr, ec] = std::from_chars(f.data(), f.data() + f.size(), out);
    if (ec == std::errc{} && ptr == f.data() + f.size()) {
        return true;
    }

    // Fallback: handle leading '+' which std::from_chars standard specifically disallows
    if (f.front() == '+' && f.size() > 1) {
        std::string_view without_plus = f.substr(1);
        auto [ptr2, ec2] = std::from_chars(without_plus.data(), without_plus.data() + without_plus.size(), out);
        return ec2 == std::errc{} && ptr2 == without_plus.data() + without_plus.size();
    }

    return false;
}

CsvScanner::CsvScanner()
    : fd_(-1), data_(nullptr), size_(0), cur_pos_(0), data_start_pos_(0), current_line_num_(0) {}

CsvScanner::~CsvScanner() {
    close();
}

const std::vector<Column>& CsvScanner::columns() const {
    return columns_;
}

int CsvScanner::columnIndex(const std::string& name) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t CsvScanner::fileSize() const {
    return size_;
}

void CsvScanner::setNeededColumns(const std::vector<size_t>& needed_cols) {
    filtering_needed_cols_ = true;
    needed_cols_mask_.assign(columns_.size(), false);
    for (size_t c : needed_cols) {
        if (c < needed_cols_mask_.size()) {
            needed_cols_mask_[c] = true;
        }
    }
}

void CsvScanner::close() {
    if (data_ && data_ != MAP_FAILED) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
    cur_pos_ = 0;
    data_start_pos_ = 0;
    current_line_num_ = 0;
    columns_.clear();
    filtering_needed_cols_ = false;
    needed_cols_mask_.clear();
    // Clearing arena drops all heap strings allocated for unescaped quoted fields
    quote_arena_.clear();
}

bool CsvScanner::open(const char* path) {
    close();

    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close();
        return false;
    }
    size_ = static_cast<size_t>(st.st_size);

    // Empty file is valid and should not fail or crash
    if (size_ == 0) {
        return true;
    }

    // Map file read-only into memory
    data_ = static_cast<char*>(mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        close();
        return false;
    }

    // Advise OS to readahead aggressively since CSV scanning is purely sequential
    madvise(data_, size_, MADV_SEQUENTIAL);

    // Read header line
    const char* header_line = nullptr;
    size_t header_len = 0;
    if (!readLine(header_line, header_len)) {
        // File has no valid lines (e.g. only newlines)
        return true;
    }

    // Parse column names from the header
    Row header_row;
    if (memchr(header_line, '"', header_len) != nullptr) {
        parseRowQuoted(header_line, header_len, header_row);
    } else {
        parseRowFast(header_line, header_len, header_row);
    }

    for (std::string_view col_sv : header_row.fields) {
        // Trim leading and trailing whitespace from column name
        while (!col_sv.empty() && (col_sv.front() == ' ' || col_sv.front() == '\t')) {
            col_sv.remove_prefix(1);
        }
        while (!col_sv.empty() && (col_sv.back() == ' ' || col_sv.back() == '\t')) {
            col_sv.remove_suffix(1);
        }
        // If column was quoted, quotes were already removed in parseRowQuoted
        columns_.push_back({std::string(col_sv), TYPE_STRING});
    }

    // Record position of the first data row
    data_start_pos_ = cur_pos_;

    // Infer column types using up to the first 10,000 data rows
    inferColumnTypes();

    // Drop temporary unescaped strings allocated during type inference
    quote_arena_.clear();

    // Reset scan cursor back to start of data rows
    cur_pos_ = data_start_pos_;
    current_line_num_ = 1;

    return true;
}

bool CsvScanner::readLine(const char*& line_start, size_t& line_len) {
    while (cur_pos_ < size_) {
        const char* current = data_ + cur_pos_;
        size_t remaining = size_ - cur_pos_;
        
        // Find newline using vectorized memchr
        const void* nl = memchr(current, '\n', remaining);
        if (nl != nullptr) {
            const char* nl_pos = static_cast<const char*>(nl);
            line_start = current;
            line_len = static_cast<size_t>(nl_pos - current);
            cur_pos_ += line_len + 1; // Advance past '\n'
            current_line_num_++;

            // Strip trailing '\r' for CRLF compatibility
            if (line_len > 0 && line_start[line_len - 1] == '\r') {
                line_len--;
            }

            // Skip fully empty lines per spec
            if (line_len == 0) {
                continue;
            }
            return true;
        } else {
            // Last line without a trailing newline
            line_start = current;
            line_len = remaining;
            cur_pos_ = size_;
            current_line_num_++;

            // Strip trailing '\r'
            if (line_len > 0 && line_start[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len == 0) {
                return false;
            }
            return true;
        }
    }
    return false;
}

void CsvScanner::parseRowFast(const char* line_start, size_t line_len, Row& row) {
    row.fields.clear();
    const char* p = line_start;
    const char* end = line_start + line_len;
    const char* field_start = p;

    // Fast path: simple scan for commas with no heap allocations
    while (p < end) {
        if (*p == ',') {
            row.fields.emplace_back(field_start, static_cast<size_t>(p - field_start));
            field_start = p + 1;
        }
        p++;
    }
    // Append the last field after the final comma
    row.fields.emplace_back(field_start, static_cast<size_t>(end - field_start));

    // Pad missing fields with empty string_view (SQL NULL)
    if (columns_.size() > 0 && row.fields.size() < columns_.size()) {
        row.fields.resize(columns_.size(), std::string_view{});
    } else if (columns_.size() > 0 && row.fields.size() > columns_.size()) {
        // Ignore extra fields beyond the header definition
        row.fields.resize(columns_.size());
    }
}

void CsvScanner::parseRowQuoted(const char* line_start, size_t line_len, Row& row) {
    row.fields.clear();
    const char* p = line_start;
    const char* end = line_start + line_len;
    size_t col_idx = 0;

    while (p <= end) {
        if (p == end) {
            // Trailing comma resulted in an empty field at the end of the line
            row.fields.emplace_back("");
            col_idx++;
            break;
        }

        if (*p == '"') {
            // Quoted field
            p++; // Skip leading quote
            const char* content_start = p;
            bool has_escaped_quote = false;
            bool terminated = false;

            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        // "" represents an escaped quote
                        has_escaped_quote = true;
                        p += 2;
                    } else {
                        // Closing quote found
                        terminated = true;
                        break;
                    }
                } else {
                    p++;
                }
            }

            if (!terminated) {
                std::cerr << "Warning: unterminated quote in CSV at line " << current_line_num_ << "\n";
            }

            size_t raw_content_len = static_cast<size_t>(p - content_start);
            if (terminated) {
                p++; // Skip closing quote
            }

            if (!has_escaped_quote) {
                // If there are no escaped quotes, point directly into mmap memory
                row.fields.emplace_back(content_start, raw_content_len);
            } else {
                bool is_needed = !filtering_needed_cols_ ||
                                 (col_idx < needed_cols_mask_.size() && needed_cols_mask_[col_idx]);
                if (!is_needed) {
                    // Field contains "" but this column is not needed by the query.
                    // Avoid allocating in quote_arena_; point directly into mmap raw content.
                    row.fields.emplace_back(content_start, raw_content_len);
                } else {
                    // Escaped quotes require unescaping into owned storage.
                    // std::deque ensures previously taken string_views are not invalidated.
                    quote_arena_.emplace_back();
                    std::string& unescaped = quote_arena_.back();
                    unescaped.reserve(raw_content_len);

                    for (size_t i = 0; i < raw_content_len; ++i) {
                        if (content_start[i] == '"' && i + 1 < raw_content_len && content_start[i + 1] == '"') {
                            unescaped.push_back('"');
                            i++; // Skip second quote
                        } else {
                            unescaped.push_back(content_start[i]);
                        }
                    }
                    row.fields.emplace_back(unescaped.data(), unescaped.size());
                }
            }
            col_idx++;

            // Consume characters up to the next comma delimiter
            while (p < end && *p != ',') {
                p++;
            }
            if (p < end && *p == ',') {
                p++; // Skip delimiter
            }
        } else {
            // Unquoted field
            const char* field_start = p;
            while (p < end && *p != ',') {
                p++;
            }
            row.fields.emplace_back(field_start, static_cast<size_t>(p - field_start));
            col_idx++;
            if (p < end && *p == ',') {
                p++; // Skip delimiter
            }
        }
    }

    // Pad missing fields with empty string_view (SQL NULL)
    if (columns_.size() > 0 && row.fields.size() < columns_.size()) {
        row.fields.resize(columns_.size(), std::string_view{});
    } else if (columns_.size() > 0 && row.fields.size() > columns_.size()) {
        // Ignore extra fields beyond the header definition
        row.fields.resize(columns_.size());
    }
}

bool CsvScanner::nextRow(Row& row) {
    const char* line_start = nullptr;
    size_t line_len = 0;

    if (!readLine(line_start, line_len)) {
        return false;
    }

    // Check if line contains double quotes to choose fast path vs slow path
    if (memchr(line_start, '"', line_len) != nullptr) {
        parseRowQuoted(line_start, line_len, row);
    } else {
        parseRowFast(line_start, line_len, row);
    }

    return true;
}

void CsvScanner::inferColumnTypes() {
    size_t num_cols = columns_.size();
    if (num_cols == 0) return;

    std::vector<bool> all_empty(num_cols, true);
    std::vector<bool> all_int(num_cols, true);
    std::vector<bool> all_num(num_cols, true);
    std::vector<bool> has_non_int_num(num_cols, false);

    Row temp_row;
    size_t rows_scanned = 0;

    while (rows_scanned < 10000) {
        const char* line_start = nullptr;
        size_t line_len = 0;
        if (!readLine(line_start, line_len)) {
            break;
        }

        if (memchr(line_start, '"', line_len) != nullptr) {
            parseRowQuoted(line_start, line_len, temp_row);
        } else {
            parseRowFast(line_start, line_len, temp_row);
        }

        rows_scanned++;

        for (size_t col = 0; col < num_cols; ++col) {
            if (col >= temp_row.fields.size()) continue;
            std::string_view val = temp_row.fields[col];
            if (isNull(val)) continue;

            all_empty[col] = false;

            long long int_val = 0;
            double double_val = 0.0;
            bool is_i = parseInt(val, int_val);
            bool is_d = is_i || parseDouble(val, double_val);

            if (!is_i) {
                all_int[col] = false;
            }
            if (is_d) {
                if (!is_i) {
                    has_non_int_num[col] = true;
                }
            } else {
                all_num[col] = false;
            }
        }
    }

    for (size_t col = 0; col < num_cols; ++col) {
        if (all_empty[col]) {
            columns_[col].type = TYPE_STRING;
        } else if (all_int[col]) {
            columns_[col].type = TYPE_INT;
        } else if (all_num[col] && has_non_int_num[col]) {
            columns_[col].type = TYPE_FLOAT;
        } else {
            columns_[col].type = TYPE_STRING;
        }
    }
}

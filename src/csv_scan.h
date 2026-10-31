#ifndef CSV_SCAN_H
#define CSV_SCAN_H

#include <string>
#include <vector>
#include <string_view>
#include <deque>
#include <cstddef>

enum ColType { TYPE_INT, TYPE_FLOAT, TYPE_STRING };

struct Column {
    std::string name;
    ColType type;
};

struct Row {
    // Reused between rows to avoid per-row heap allocations
    std::vector<std::string_view> fields;
};

class CsvScanner {
public:
    CsvScanner();
    ~CsvScanner();

    // mmap, read header, infer types from the first 10,000 data rows
    bool open(const char* path);

    // List of inferred columns
    const std::vector<Column>& columns() const;

    // Advances to next row, populating row.fields. Returns false at EOF.
    bool nextRow(Row& row);

    // Returns column index by name, or -1 if missing
    int columnIndex(const std::string& name) const;

    // Unmaps memory and releases resources
    void close();

    // Raw file size in bytes
    size_t fileSize() const;

private:
    int fd_;
    char* data_;
    size_t size_;
    size_t cur_pos_;
    size_t data_start_pos_; // Position where first data row starts (after header)
    size_t current_line_num_;

    std::vector<Column> columns_;

    // Arena for unescaped quoted fields. std::deque guarantees pointer stability
    // across push_back operations so string_view references remain valid until close().
    std::deque<std::string> quote_arena_;

    // Helpers
    bool readLine(const char*& line_start, size_t& line_len);
    void parseRowFast(const char* line_start, size_t line_len, Row& row);
    void parseRowQuoted(const char* line_start, size_t line_len, Row& row);
    void inferColumnTypes();
};

// Returns true if field is empty (empty field == SQL NULL)
bool isNull(std::string_view f);

// Parses integer using std::from_chars
bool parseInt(std::string_view f, long long& out);

// Parses floating point number using std::from_chars
bool parseDouble(std::string_view f, double& out);

#endif // CSV_SCAN_H

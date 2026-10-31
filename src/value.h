#ifndef VALUE_H
#define VALUE_H

#include "csv_scan.h"

#include <string_view>
#include <string>
#include <cstdio>

enum ValueType {
    V_NULL,
    V_INT,
    V_FLOAT,
    V_STRING
};

struct Value {
    ValueType type;
    long long i;
    double d;
    std::string_view s; // points into mmap/arena or BoundExpr literal storage
};

// Value constructors
inline Value makeNull() {
    Value v;
    v.type = V_NULL;
    v.i = 0;
    v.d = 0.0;
    v.s = std::string_view();
    return v;
}

inline Value makeInt(long long val) {
    Value v;
    v.type = V_INT;
    v.i = val;
    v.d = 0.0;
    v.s = std::string_view();
    return v;
}

inline Value makeFloat(double val) {
    Value v;
    v.type = V_FLOAT;
    v.i = 0;
    v.d = val;
    v.s = std::string_view();
    return v;
}

inline Value makeString(std::string_view val) {
    Value v;
    v.type = V_STRING;
    v.i = 0;
    v.d = 0.0;
    v.s = val;
    return v;
}

// Check truthiness according to SQLite rules
bool isTruthy(Value v);

// Convert value to number (longest numeric prefix for strings)
Value toNumber(Value v);

// Exact comparison between int64 and double
int compareIntAndDouble(long long i, double d);

// Compare two non-null values according to SQLite ordering (NUMERIC < STRING)
// Returns -1 if a < b, 0 if a == b, 1 if a > b
int compareValues(Value a, Value b);

// Convert a CSV field to a Value based on inferred column type
Value valueFromCsvField(std::string_view field, ColType colType);

// Format a Value for CSV output printer
void formatValue(Value v, FILE* out);

// Format a Value into a fixed stack buffer without heap allocation
size_t formatValueToBuffer(Value v, char* buf, size_t buf_size);

// Format a Value into a std::string (used by tests and printer)
std::string formatValueToString(Value v);

#endif // VALUE_H

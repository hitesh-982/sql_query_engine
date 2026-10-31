#include "value.h"

#include <cmath>
#include <cstring>
#include <charconv>
#include <limits>
#include <cstdlib>

// Helper to check for ASCII space
static inline bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Helper to check for ASCII digit
static inline bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isTruthy(Value v) {
    switch (v.type) {
        case V_NULL:
            return false;
        case V_INT:
            return v.i != 0;
        case V_FLOAT:
            return v.d != 0.0 && !std::isnan(v.d);
        case V_STRING: {
            Value num = toNumber(v);
            return isTruthy(num);
        }
    }
    return false;
}

Value toNumber(Value v) {
    if (v.type == V_INT || v.type == V_FLOAT) {
        return v;
    }
    if (v.type == V_NULL) {
        return makeNull();
    }

    // Parse longest numeric prefix for string
    std::string_view s = v.s;
    size_t idx = 0;
    while (idx < s.size() && isAsciiSpace(s[idx])) {
        idx++;
    }
    if (idx >= s.size()) {
        return makeInt(0);
    }

    size_t start = idx;
    if (s[idx] == '+' || s[idx] == '-') {
        idx++;
    }

    size_t digits_before = 0;
    while (idx < s.size() && isAsciiDigit(s[idx])) {
        digits_before++;
        idx++;
    }

    bool has_dot = false;
    size_t digits_after = 0;
    if (idx < s.size() && s[idx] == '.') {
        // Dot is only part of number if digits exist before it or immediately after it
        if (digits_before > 0 || (idx + 1 < s.size() && isAsciiDigit(s[idx + 1]))) {
            has_dot = true;
            idx++;
            while (idx < s.size() && isAsciiDigit(s[idx])) {
                digits_after++;
                idx++;
            }
        }
    }

    if (digits_before + digits_after == 0) {
        // No valid numeric digits found (e.g. "abc", "-", ".")
        return makeInt(0);
    }

    bool has_exp = false;
    if (idx < s.size() && (s[idx] == 'e' || s[idx] == 'E')) {
        size_t exp_idx = idx + 1;
        if (exp_idx < s.size() && (s[exp_idx] == '+' || s[exp_idx] == '-')) {
            exp_idx++;
        }
        // Exponent must have at least one digit to be valid
        if (exp_idx < s.size() && isAsciiDigit(s[exp_idx])) {
            has_exp = true;
            idx = exp_idx;
            while (idx < s.size() && isAsciiDigit(s[idx])) {
                idx++;
            }
        }
    }

    // The longest numeric prefix is in s[start .. idx - 1]
    std::string_view prefix = s.substr(start, idx - start);

    if (has_dot || has_exp) {
        // Parse as float using null-terminated string or strtod
        std::string str(prefix);
        char* endptr = nullptr;
        double val = std::strtod(str.c_str(), &endptr);
        return makeFloat(val);
    } else {
        // Parse as integer, with overflow check promoting to float
        long long iv = 0;
        auto res = std::from_chars(prefix.data(), prefix.data() + prefix.size(), iv);
        if (res.ec == std::errc::result_out_of_range) {
            std::string str(prefix);
            double val = std::strtod(str.c_str(), nullptr);
            return makeFloat(val);
        }
        return makeInt(iv);
    }
}

int compareIntAndDouble(long long i, double d) {
    if (std::isnan(d)) {
        // SQLite treats NaN as float
        return 0;
    }
    // 2^63 is 9223372036854775808.0
    constexpr double two_pow_63 = 9223372036854775808.0;
    if (d >= two_pow_63) {
        // Double is larger than any int64 can represent
        return -1;
    }
    if (d < -two_pow_63) {
        // Double is smaller than any int64 can represent
        return 1;
    }

    // Within int64 range: truncate double to long long
    long long d_int = static_cast<long long>(d);
    if (i < d_int) {
        return -1;
    }
    if (i > d_int) {
        return 1;
    }

    // Integer parts are equal: check fractional difference
    double frac = d - static_cast<double>(d_int);
    if (frac > 0.0) {
        return -1; // d has positive fraction, so i < d
    }
    if (frac < 0.0) {
        return 1;  // d has negative fraction, so i > d
    }
    return 0;
}

int compareValues(Value a, Value b) {
    // Both are numeric
    bool a_num = (a.type == V_INT || a.type == V_FLOAT);
    bool b_num = (b.type == V_INT || b.type == V_FLOAT);

    if (a_num && b_num) {
        if (a.type == V_INT && b.type == V_INT) {
            if (a.i < b.i) return -1;
            if (a.i > b.i) return 1;
            return 0;
        }
        if (a.type == V_FLOAT && b.type == V_FLOAT) {
            if (a.d < b.d) return -1;
            if (a.d > b.d) return 1;
            return 0;
        }
        if (a.type == V_INT && b.type == V_FLOAT) {
            return compareIntAndDouble(a.i, b.d);
        }
        // a is FLOAT, b is INT
        return -compareIntAndDouble(b.i, a.d);
    }

    // Both are strings
    if (a.type == V_STRING && b.type == V_STRING) {
        size_t min_len = a.s.size() < b.s.size() ? a.s.size() : b.s.size();
        int cmp = std::memcmp(a.s.data(), b.s.data(), min_len);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
        if (a.s.size() < b.s.size()) return -1;
        if (a.s.size() > b.s.size()) return 1;
        return 0;
    }

    // SQLite ordering: NUMERIC < STRING
    if (a_num && b.type == V_STRING) {
        return -1;
    }
    if (a.type == V_STRING && b_num) {
        return 1;
    }

    return 0;
}

Value valueFromCsvField(std::string_view field, ColType colType) {
    if (isNull(field)) {
        return makeNull();
    }

    switch (colType) {
        case TYPE_STRING:
            return makeString(field);
        case TYPE_INT: {
            long long iv;
            if (parseInt(field, iv)) {
                return makeInt(iv);
            }
            double dv;
            if (parseDouble(field, dv)) {
                return makeFloat(dv);
            }
            return makeString(field);
        }
        case TYPE_FLOAT: {
            double dv;
            if (parseDouble(field, dv)) {
                return makeFloat(dv);
            }
            return makeString(field);
        }
    }
    return makeString(field);
}

void formatValue(Value v, FILE* out) {
    switch (v.type) {
        case V_NULL:
            return;
        case V_INT:
            std::fprintf(out, "%lld", v.i);
            return;
        case V_FLOAT: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", v.d);
            bool has_special = false;
            for (char* p = buf; *p; ++p) {
                if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') {
                    has_special = true;
                    break;
                }
            }
            if (!has_special) {
                std::fputs(buf, out);
                std::fputs(".0", out);
            } else {
                std::fputs(buf, out);
            }
            return;
        }
        case V_STRING: {
            bool needs_quotes = false;
            for (char c : v.s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                    needs_quotes = true;
                    break;
                }
            }
            if (!needs_quotes) {
                std::fwrite(v.s.data(), 1, v.s.size(), out);
            } else {
                std::fputc('"', out);
                for (char c : v.s) {
                    if (c == '"') {
                        std::fputs("\"\"", out);
                    } else {
                        std::fputc(c, out);
                    }
                }
                std::fputc('"', out);
            }
            return;
        }
    }
}

std::string formatValueToString(Value v) {
    switch (v.type) {
        case V_NULL:
            return "";
        case V_INT:
            return std::to_string(v.i);
        case V_FLOAT: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", v.d);
            bool has_special = false;
            for (char* p = buf; *p; ++p) {
                if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') {
                    has_special = true;
                    break;
                }
            }
            if (!has_special) {
                return std::string(buf) + ".0";
            }
            return std::string(buf);
        }
        case V_STRING:
            return std::string(v.s);
    }
    return "";
}

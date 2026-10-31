#include "group_key.h"

#include <cmath>
#include <cstring>
#include <cstdint>

void encodeGroupKey(const Value* values, size_t count, std::string& buf) {
    buf.clear();
    constexpr double two_pow_63 = 9223372036854775808.0;

    for (size_t idx = 0; idx < count; ++idx) {
        const Value& v = values[idx];
        switch (v.type) {
            case V_NULL:
                buf.push_back('N');
                break;
            case V_INT: {
                buf.push_back('I');
                buf.append(reinterpret_cast<const char*>(&v.i), sizeof(long long));
                break;
            }
            case V_FLOAT: {
                // Range check double before casting to integer
                if (!std::isnan(v.d) && !std::isinf(v.d) && v.d >= -two_pow_63 && v.d < two_pow_63) {
                    long long d_int = static_cast<long long>(v.d);
                    if (static_cast<double>(d_int) == v.d) {
                        // Integral float: encode as 'I' so 1.0 matches 1
                        buf.push_back('I');
                        buf.append(reinterpret_cast<const char*>(&d_int), sizeof(long long));
                        break;
                    }
                }
                // Double value: normalize -0.0 to +0.0
                double d = v.d;
                if (d == 0.0) {
                    d = 0.0;
                }
                buf.push_back('F');
                buf.append(reinterpret_cast<const char*>(&d), sizeof(double));
                break;
            }
            case V_STRING: {
                buf.push_back('S');
                uint32_t len = static_cast<uint32_t>(v.s.size());
                buf.append(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
                buf.append(v.s.data(), v.s.size());
                break;
            }
        }
    }
}

void encodeGroupKey(const std::vector<Value>& values, std::string& buf) {
    encodeGroupKey(values.data(), values.size(), buf);
}


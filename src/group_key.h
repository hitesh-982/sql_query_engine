#ifndef GROUP_KEY_H
#define GROUP_KEY_H

#include "value.h"

#include <string>
#include <vector>
#include <cstddef>

// Encode composite key over an array of Values into buf.
// Uses type tag byte + payload:
// - V_NULL: tag 'N'
// - V_INT: tag 'I' + 8-byte long long
// - V_FLOAT: integral float in [-2^63, 2^63) encoded as 'I' with long long (so 1.0 matches 1),
//            -0.0 normalized to +0.0, 'F' + 8-byte double
// - V_STRING: tag 'S' + 4-byte uint32_t length + bytes
void encodeGroupKey(const Value* values, size_t count, std::string& buf);

// Convenience wrapper for vector of Values
void encodeGroupKey(const std::vector<Value>& values, std::string& buf);

#endif // GROUP_KEY_H


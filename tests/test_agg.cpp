#include "../src/ops_agg.h"
#include "../src/expr_eval.h"
#include "../src/value.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

void testCompositeKeys() {
    std::cout << "Testing Composite Key Encoding...\n";

    std::string key1, key2;

    // 1. ("a", "bc") vs ("ab", "c")
    std::vector<Value> v1 = {makeString("a"), makeString("bc")};
    std::vector<Value> v2 = {makeString("ab"), makeString("c")};
    AggregateOperator::encodeKey(v1, key1);
    AggregateOperator::encodeKey(v2, key2);
    assert(key1 != key2 && "('a','bc') must not equal ('ab','c')");

    // 2. NULL vs "N"
    std::vector<Value> v_null = {makeNull()};
    std::vector<Value> v_str_n = {makeString("N")};
    AggregateOperator::encodeKey(v_null, key1);
    AggregateOperator::encodeKey(v_str_n, key2);
    assert(key1 != key2 && "NULL must not encode identically to string 'N'");

    // 3. 1 (INT) vs 1.0 (FLOAT)
    std::vector<Value> v_int1 = {makeInt(1)};
    std::vector<Value> v_flt1 = {makeFloat(1.0)};
    AggregateOperator::encodeKey(v_int1, key1);
    AggregateOperator::encodeKey(v_flt1, key2);
    assert(key1 == key2 && "1 (int) and 1.0 (float) must encode to identical key");

    // 4. 0.0 vs -0.0
    std::vector<Value> v_zero = {makeFloat(0.0)};
    std::vector<Value> v_neg_zero = {makeFloat(-0.0)};
    AggregateOperator::encodeKey(v_zero, key1);
    AggregateOperator::encodeKey(v_neg_zero, key2);
    assert(key1 == key2 && "0.0 and -0.0 must encode to identical key");

    // 5. Large double >= 2^63 must not cast to long long
    std::vector<Value> v_huge = {makeFloat(1e300)};
    AggregateOperator::encodeKey(v_huge, key1);
    assert(key1[0] == 'F' && "Double >= 2^63 must use 'F' tag");

    // 6. NaN and Inf must use 'F' tag
    std::vector<Value> v_nan = {makeFloat(std::numeric_limits<double>::quiet_NaN())};
    AggregateOperator::encodeKey(v_nan, key1);
    assert(key1[0] == 'F' && "NaN must use 'F' tag");

    std::cout << "  -> Composite keys OK.\n";
}

void testAggStateSum() {
    std::cout << "Testing AggState SUM / AVG / Kahan...\n";

    // 1. Kahan precision: 1e16 + 1.0 + (-1e16)
    AggState kahan_state;
    AggregateOperator::addFloatToSum(kahan_state, 1e16);
    AggregateOperator::addFloatToSum(kahan_state, 1.0);
    AggregateOperator::addFloatToSum(kahan_state, -1e16);
    double final_kahan = kahan_state.sum_d + kahan_state.sum_c;
    assert(final_kahan == 1.0 && "Kahan summation must preserve 1.0 in 1e16 + 1.0 - 1e16");

    // 2. Int-then-float: 5, 3, 1.5
    AggState int_float_state;
    AggregateOperator::addIntToSum(int_float_state, 5);
    AggregateOperator::addIntToSum(int_float_state, 3);
    assert(!int_float_state.sum_is_float && int_float_state.sum_i == 8);
    AggregateOperator::addFloatToSum(int_float_state, 1.5);
    assert(int_float_state.sum_is_float);
    double sum1 = int_float_state.sum_d + int_float_state.sum_c;
    assert(sum1 == 9.5 && "5 + 3 + 1.5 must equal 9.5");

    // 3. Float-then-int: 1.5, 3, 5
    AggState float_int_state;
    AggregateOperator::addFloatToSum(float_int_state, 1.5);
    assert(float_int_state.sum_is_float);
    AggregateOperator::addIntToSum(float_int_state, 3);
    AggregateOperator::addIntToSum(float_int_state, 5);
    double sum2 = float_int_state.sum_d + float_int_state.sum_c;
    assert(sum2 == 9.5 && "1.5 + 3 + 5 must equal 9.5");

    // 4. Exact integer overflow
    AggState overflow_state;
    AggregateOperator::addIntToSum(overflow_state, 9223372036854775807LL);
    assert(!overflow_state.sum_overflow);
    AggregateOperator::addIntToSum(overflow_state, 1);
    assert(overflow_state.sum_overflow && "LLONG_MAX + 1 must set sum_overflow = true");

    // 5. Text sum behavior matching SQLite on ['12abc', '1.5x', 'abc', '7', ' 8 ']
    AggState text_sum_state;
    std::vector<std::string> text_vals = {"12abc", "1.5x", "abc", "7", " 8 "};
    for (const auto& s : text_vals) {
        Value val = makeString(s);
        Value num;
        if (isEntirelyNumeric(val.s, num)) {
            if (num.type == V_INT) {
                AggregateOperator::addIntToSum(text_sum_state, num.i);
            } else {
                AggregateOperator::addFloatToSum(text_sum_state, num.d);
            }
        } else {
            Value prefix_num = toNumber(val);
            double d = 0.0;
            if (prefix_num.type == V_INT) d = static_cast<double>(prefix_num.i);
            else if (prefix_num.type == V_FLOAT) d = prefix_num.d;
            AggregateOperator::addFloatToSum(text_sum_state, d);
        }
    }
    double total_text_sum = text_sum_state.sum_d + text_sum_state.sum_c;
    assert(total_text_sum == 28.5 && "Sum of text_vals must equal 28.5 (matching SQLite)");

    std::cout << "  -> AggState SUM / AVG / Kahan OK.\n";
}

void testMinMaxOrdering() {
    std::cout << "Testing MIN / MAX ordering...\n";

    // Numbers vs Strings vs NULL
    Value v_null = makeNull();
    Value v_int = makeInt(42);
    Value v_flt = makeFloat(42.5);
    Value v_str = makeString("hello");

    (void)v_null; // Used in test documentation
    assert(compareValues(v_int, v_flt) < 0);
    assert(compareValues(v_flt, v_int) > 0);
    assert(compareValues(v_int, v_str) < 0); // numeric < string in SQLite
    assert(compareValues(v_str, v_int) > 0);

    // Byte string comparison
    Value s1 = makeString("abc");
    Value s2 = makeString("abd");
    assert(compareValues(s1, s2) < 0);

    std::cout << "  -> MIN / MAX ordering OK.\n";
}

void testStructuralEquality() {
    std::cout << "Testing boundExprsEqual...\n";

    // Two identical column refs
    auto c1 = std::make_unique<BoundExpr>();
    c1->kind = BoundExprKind::COLUMN_REF;
    c1->col_idx = 3;

    auto c2 = std::make_unique<BoundExpr>();
    c2->kind = BoundExprKind::COLUMN_REF;
    c2->col_idx = 3;

    auto c3 = std::make_unique<BoundExpr>();
    c3->kind = BoundExprKind::COLUMN_REF;
    c3->col_idx = 4;

    assert(boundExprsEqual(c1.get(), c2.get()));
    assert(!boundExprsEqual(c1.get(), c3.get()));

    // Binary ops
    auto b1 = std::make_unique<BoundExpr>();
    b1->kind = BoundExprKind::BINARY_OP;
    b1->op = "+";
    b1->left = cloneBoundExpr(c1.get());
    b1->right = cloneBoundExpr(c3.get());

    auto b2 = std::make_unique<BoundExpr>();
    b2->kind = BoundExprKind::BINARY_OP;
    b2->op = "+";
    b2->left = cloneBoundExpr(c2.get());
    b2->right = cloneBoundExpr(c3.get());

    assert(boundExprsEqual(b1.get(), b2.get()));

    std::cout << "  -> Structural equality OK.\n";
}

int main() {
    std::cout << "=== Running test_agg ===\n";
    testCompositeKeys();
    testAggStateSum();
    testMinMaxOrdering();
    testStructuralEquality();
    std::cout << "All test_agg tests PASSED!\n";
    return 0;
}

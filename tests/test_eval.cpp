#include "../src/value.h"
#include "../src/expr_eval.h"
#include "../src/operator.h"
#include "../src/sql_parser.h"
#include "../src/planner.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <deque>
#include <chrono>

static void test_truth_tables() {
    Tuple t;
    std::deque<std::string> arena;
    TableRef tbl{"t", ""};
    std::vector<Column> schema;

    Value v_true = makeInt(1);
    Value v_false = makeInt(0);
    Value v_null = makeNull();

    // NOT
    {
        auto e_not_t = std::make_unique<BoundExpr>();
        e_not_t->kind = BoundExprKind::UNARY_OP; e_not_t->op = "not";
        e_not_t->left = std::make_unique<BoundExpr>();
        e_not_t->left->kind = BoundExprKind::LITERAL; e_not_t->left->lit_val = v_true;
        Value res = evalExpr(e_not_t.get(), t);
        assert(res.type == V_INT && res.i == 0);

        auto e_not_f = std::make_unique<BoundExpr>();
        e_not_f->kind = BoundExprKind::UNARY_OP; e_not_f->op = "not";
        e_not_f->left = std::make_unique<BoundExpr>();
        e_not_f->left->kind = BoundExprKind::LITERAL; e_not_f->left->lit_val = v_false;
        res = evalExpr(e_not_f.get(), t);
        assert(res.type == V_INT && res.i == 1);

        auto e_not_n = std::make_unique<BoundExpr>();
        e_not_n->kind = BoundExprKind::UNARY_OP; e_not_n->op = "not";
        e_not_n->left = std::make_unique<BoundExpr>();
        e_not_n->left->kind = BoundExprKind::LITERAL; e_not_n->left->lit_val = v_null;
        res = evalExpr(e_not_n.get(), t);
        assert(res.type == V_NULL);
    }

    // AND truth table
    auto test_and = [&](Value a, Value b, Value expected) {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "and";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = a;
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = b;
        Value r = evalExpr(e.get(), t);
        if (expected.type == V_NULL) {
            assert(r.type == V_NULL);
        } else {
            assert(r.type == expected.type && r.i == expected.i);
        }
    };

    test_and(v_true, v_true, makeInt(1));
    test_and(v_true, v_false, makeInt(0));
    test_and(v_true, v_null, makeNull());
    test_and(v_false, v_true, makeInt(0));
    test_and(v_false, v_false, makeInt(0));
    test_and(v_false, v_null, makeInt(0));
    test_and(v_null, v_true, makeNull());
    test_and(v_null, v_false, makeInt(0));
    test_and(v_null, v_null, makeNull());

    // OR truth table
    auto test_or = [&](Value a, Value b, Value expected) {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "or";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = a;
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = b;
        Value r = evalExpr(e.get(), t);
        if (expected.type == V_NULL) {
            assert(r.type == V_NULL);
        } else {
            assert(r.type == expected.type && r.i == expected.i);
        }
    };

    test_or(v_true, v_true, makeInt(1));
    test_or(v_true, v_false, makeInt(1));
    test_or(v_true, v_null, makeInt(1));
    test_or(v_false, v_true, makeInt(1));
    test_or(v_false, v_false, makeInt(0));
    test_or(v_false, v_null, makeNull());
    test_or(v_null, v_true, makeInt(1));
    test_or(v_null, v_false, makeNull());
    test_or(v_null, v_null, makeNull());

    // Normalised return: SELECT qty AND 5 FROM t (qty = 10 -> returns 1, not 5!)
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "and";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(10);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(5);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 1); // Normalized to 1!
    }

    std::cout << "[OK] Truth tables passed\n";
}

static void test_null_and_in() {
    Tuple t;

    // NULL = NULL is NULL
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "=";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeNull();
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeNull();
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_NULL);
    }

    // NULL IS NULL is 1
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::IS_NULL; e->is_not = false;
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeNull();
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 1);
    }

    // 1 IN (2, NULL) is NULL
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::IN_LIST;
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(1);
        auto item1 = std::make_unique<BoundExpr>(); item1->kind = BoundExprKind::LITERAL; item1->lit_val = makeInt(2);
        auto item2 = std::make_unique<BoundExpr>(); item2->kind = BoundExprKind::LITERAL; item2->lit_val = makeNull();
        e->in_list.push_back(std::move(item1));
        e->in_list.push_back(std::move(item2));
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_NULL);
    }

    // 2 IN (2, NULL) is 1
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::IN_LIST;
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(2);
        auto item1 = std::make_unique<BoundExpr>(); item1->kind = BoundExprKind::LITERAL; item1->lit_val = makeInt(2);
        auto item2 = std::make_unique<BoundExpr>(); item2->kind = BoundExprKind::LITERAL; item2->lit_val = makeNull();
        e->in_list.push_back(std::move(item1));
        e->in_list.push_back(std::move(item2));
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 1);
    }

    std::cout << "[OK] NULL and IN tests passed\n";
}

static void test_arithmetic() {
    Tuple t;

    // 5 / 2 = 2
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "/";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(5);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(2);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 2);
    }

    // 5 / 2.0 = 2.5
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "/";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(5);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeFloat(2.0);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_FLOAT && std::abs(r.d - 2.5) < 1e-9);
    }

    // -5 / 2 = -2
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "/";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(-5);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(2);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == -2);
    }

    // 1 / 0 is NULL
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "/";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(1);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(0);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_NULL);
    }

    // 1.0 / 0 is NULL
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "/";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeFloat(1.0);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(0);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_NULL);
    }

    // 9223372036854775807 + 1 becomes a FLOAT
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "+";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(9223372036854775807LL);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(1);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_FLOAT && r.d == 9223372036854775808.0);
    }

    // -(-9223372036854775807 - 1) becomes a FLOAT
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::UNARY_OP; e->op = "-";
        e->left = std::make_unique<BoundExpr>();
        e->left->kind = BoundExprKind::LITERAL;
        e->left->lit_val = makeInt(std::numeric_limits<long long>::min());
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_FLOAT && r.d == 9223372036854775808.0);
    }

    // 'abc' + 1 = 1
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "+";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeString("abc");
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(1);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 1);
    }

    // '12abc' + 1 = 13
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = "+";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeString("12abc");
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(1);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 13);
    }

    std::cout << "[OK] Arithmetic tests passed\n";
}

static void test_comparisons_and_affinity() {
    Tuple t;

    // 10 > 9 is 1
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = ">";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeInt(10);
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeInt(9);
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 1);
    }

    // '10' > '9' is 0 (string compare)
    {
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = ">";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::LITERAL; e->left->lit_val = makeString("10");
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL; e->right->lit_val = makeString("9");
        Value r = evalExpr(e.get(), t);
        assert(r.type == V_INT && r.i == 0);
    }

    // INT column > '9' compares numerically
    {
        Tuple tup;
        tup.values.push_back(makeInt(10)); // col 0
        auto e = std::make_unique<BoundExpr>();
        e->kind = BoundExprKind::BINARY_OP; e->op = ">";
        e->left = std::make_unique<BoundExpr>(); e->left->kind = BoundExprKind::COLUMN_REF;
        e->left->col_idx = 0; e->left->affinity = Affinity::NUMERIC;
        e->right = std::make_unique<BoundExpr>(); e->right->kind = BoundExprKind::LITERAL;
        e->right->lit_val = makeString("9"); e->right->affinity = Affinity::NONE;
        Value r = evalExpr(e.get(), tup);
        assert(r.type == V_INT && r.i == 1);
    }

    // Exact int vs float comparisons
    assert(compareIntAndDouble(9223372036854775807LL, 9223372036854775808.0) == -1); // int is smaller than 2^63
    assert(compareIntAndDouble(-9223372036854775807LL - 1LL, -1e19) == 1); // int is larger than double < -2^63
    assert(compareIntAndDouble(-9223372036854775807LL - 1LL, -9223372036854775808.0) == 0); // LLONG_MIN == -2^63
    assert(compareIntAndDouble(5, 5.0) == 0);
    assert(compareIntAndDouble(5, 5.1) == -1);
    assert(compareIntAndDouble(5, 4.9) == 1);
    assert(compareIntAndDouble(-5, -5.0) == 0);
    assert(compareIntAndDouble(-5, -5.1) == 1);
    assert(compareIntAndDouble(-5, -4.9) == -1);

    std::cout << "[OK] Comparison and exact affinity tests passed\n";
}

static void test_like() {
    assert(likeMatch("abc", "A%") == true);
    assert(likeMatch("abc", "_b_") == true);
    assert(likeMatch("", "%") == true);
    assert(likeMatch("abc", "%%%%%a") == false);

    // Fast finish on '%%%%%a' on long string
    std::string long_str(100000, 'x');
    auto t0 = std::chrono::steady_clock::now();
    assert(likeMatch(long_str, "%%%%%a") == false);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    assert(ms < 50.0); // finishes instantly, no blowup

    // UTF-8 test: 'é' LIKE '_' is 1
    // 'é' in UTF-8 is "\xC3\xA9" (2 bytes)
    std::string e_acute = "\xC3\xA9";
    assert(likeMatch(e_acute, "_") == true);
    assert(likeMatch(e_acute, "__") == false);

    std::cout << "[OK] LIKE tests passed\n";
}

static void test_to_number() {
    // toNumber('1e') is 1
    Value v1 = toNumber(makeString("1e"));
    assert(v1.type == V_INT && v1.i == 1);

    // toNumber('1e5') is float
    Value v2 = toNumber(makeString("1e5"));
    assert(v2.type == V_FLOAT && v2.d == 100000.0);

    // toNumber('0x10') is 0
    Value v3 = toNumber(makeString("0x10"));
    assert(v3.type == V_INT && v3.i == 0);

    // Leading whitespace
    Value v4 = toNumber(makeString("   -42.5xyz"));
    assert(v4.type == V_FLOAT && v4.d == -42.5);

    std::cout << "[OK] toNumber tests passed\n";
}

int main() {
    std::cout << "=== Running test_eval Unit Tests ===\n";
    test_truth_tables();
    test_null_and_in();
    test_arithmetic();
    test_comparisons_and_affinity();
    test_like();
    test_to_number();
    std::cout << "All test_eval tests PASSED!\n";
    return 0;
}
